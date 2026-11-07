#include "SpSigningProbe.h"

#include "AppController.h"
#include "PriceProbe.h"
#include "ViewServices.h"
#include "account/services/AccountServices.h"
#include "account/services/Conversions.h"
#include "account/services/ServiceAdapters.h"
#include "account/views/Send.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "interfaces/backend/provider.h"
#include "interfaces/backend/transaction.h"
#include "modules/sp/SpPlugin.h"
#include "modules/sp/SpSigning.h"
#include <QCheckBox>
#include <QLineEdit>
#include <QTimer>
#include <Qontrol>
#include <common.h>
#include <memory>

namespace smoke {
namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("SP signing smoke: %s", message);
    }
}

class SpSend final : public account::view::Send {
public:
    using Send::Send;

    auto transaction() -> backend::TransactionTemplate {
        auto tx = txTemplate();
        require(tx.has_value(), "Send has a valid template");
        return tx.value();
    }
};

auto input(SpSend &send, const QString &placeholder) -> QLineEdit * {
    for (auto *field : send.findChildren<QLineEdit *>()) {
        if (field->placeholderText() == placeholder) {
            return field;
        }
    }
    qFatal("SP signing smoke input missing");
}

// What the funded account crafts and finalizes, through its own transaction service. A coin
// selection runs on the account at once: the service runs it on a controller worker, which
// this controller holds back.
void craftOnAccount(ViewServices &services, SpController &controller,
                    backend::ITransaction *transactions) {
    services.transaction.crafting = [&controller,
                                     transactions](const backend::TransactionTemplate &tx) {
        if (!tx.inputs.isEmpty()) {
            return transactions->craft(tx);
        }
        return account::services::toCraftResult(
            controller.getAccount().value()->select_transaction_coins(
                account::services::toTemplate(tx)));
    };
    services.transaction.finalizing = [transactions](const backend::TransactionTemplate &tx) {
        return transactions->finalize(tx);
    };
}

} // namespace

void spSigningSmoke(const QString &name) {
    auto *host = Host::get();
    PricePlugin feedPlugin;
    DeferredKeys keys;
    AppController::get()->registerBuiltinPlugin(std::make_unique<SpPlugin>());
    auto *registry = AppController::get()->pluginRegistry();
    registry->registerBuiltin(&feedPlugin);
    PriceFeed feed;
    feed.has_default = true;
    PriceInstance instance;
    instance.instanceId = QStringLiteral("sp-signing-fees");
    instance.provider = &feed;
    host->registerInstance(instance.id(), &instance, &feedPlugin.module);
    host->selectFees({.mode = FeeMode::Default, .source = host->feeSources().first().id});
    SpController controller(name);
    // The fixture controller serves no account: its account's own service context, which
    // the controller owns.
    auto *context = new account::AccountServices<AccountController, Account>(name, &controller);
    std::unique_ptr<backend::IService> adapter(
        account::serviceAdapter(backend::ServiceKind::Transaction, context, nullptr));
    auto *transactions = qobject_cast<backend::ITransaction *>(adapter.get());
    require(transactions != nullptr, "the account's transaction service is an ITransaction");
    ViewServices services;
    craftOnAccount(services, controller, transactions);
    SpSend send(services.services(), new SpSigning(&controller, services.services()));
    auto *signing = send.findChild<SpSigning *>();
    require(signing != nullptr, "uses the real SP signing widget");
    auto address = QString::fromStdString(std::string(controller.getSpAddress()));
    input(send, TR("send-placeholder-address"))->setText(address);
    for (auto *checkbox : send.findChildren<QCheckBox *>()) {
        if (checkbox->property("outputId").isValid()) {
            checkbox->setChecked(true);
        }
    }
    auto *amount = input(send, TR("send-placeholder-amount"));
    auto *fee = send.findChild<QLineEdit *>(QStringLiteral("fee-value"));
    require(send.transaction().feeRate == 0.5 && send.transaction().outputs[0].maximum,
            "real funded SP Send starts at 0.5 sat/vB with MAX");
    auto simulation = controller.getAccount().value()->simulate_transaction(
        account::services::toTemplate(send.transaction()));
    require(simulation.is_valid && simulation.fee > 0, "real funded template constructs");
    auto frozenAmount = amount->text();
    signing->onSignClicked();
    require(controller.workers.size() == 1 && !fee->isVisibleTo(&send) && !amount->isEnabled(),
            "successful actual preparation locks before submitting validation");
    auto crafts = services.transaction.crafts();
    emit feed.defaultFeeRate(1, 5);
    require(send.transaction().feeRate == 0.5 && amount->text() == frozenAmount &&
                services.transaction.crafts() == crafts,
            "pending validation push cannot change fee MAX or simulations");
    signing->onSignClicked();
    require(controller.workers.size() == 1, "pending validation refuses another preparation");
    controller.discardWorkers();

    ModalAnswer answer;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &answer, &ModalAnswer::onTimeout, qontrol::UNIQUE);
    timer.start(1);
    PsbtValidation validation{};
    validation.is_ok = true;
    validation.is_valid = true;
    signing->onValidationResult(validation);
    require(answer.answers == 1 && fee->isVisibleTo(&send) && send.transaction().feeRate == 5,
            "missing keys releases prepared state and follows the cached rate");

    host->setKeys(&keys, &feedPlugin.module);
    signing->onSignClicked();
    controller.discardWorkers();
    signing->onValidationResult(validation);
    require(keys.requests == 1 && !keys.psbt.isEmpty() && !fee->isVisibleTo(&send),
            "retry reaches the actual deferred signing request while locked");
    frozenAmount = amount->text();
    auto prepared = keys.psbt;
    crafts = services.transaction.crafts();
    emit feed.defaultFeeRate(1, 7);
    require(send.transaction().feeRate == 5 && amount->text() == frozenAmount &&
                services.transaction.crafts() == crafts && keys.psbt == prepared &&
                keys.requests == 1,
            "pending signer push cannot change fee MAX simulation or prepared PSBT");
    emit keys.cancelled(keys.request);
    require(fee->isVisibleTo(&send) && send.transaction().feeRate == 7,
            "signer cancellation releases state and catches up");

    signing->onSignClicked();
    controller.discardWorkers();
    validation.spent_input_count = 1;
    signing->onValidationResult(validation);
    require(answer.answers == 2 && fee->isVisibleTo(&send) && keys.requests == 1,
            "spent-input refusal unlocks without starting a signer");
    validation.spent_input_count = 0;
    validation.reused_output_count = 1;
    answer.accept = false;
    signing->onSignClicked();
    controller.discardWorkers();
    signing->onValidationResult(validation);
    require(answer.answers == 3 && fee->isVisibleTo(&send) && keys.requests == 1,
            "reuse-warning rejection releases the prepared transaction");
    validation.reused_output_count = 0;
    answer.accept = true;

    signing->onSignClicked();
    controller.discardWorkers();
    signing->onValidationResult(validation);
    emit keys.error(keys.request, QStringLiteral("fixture signing failure"));
    require(answer.answers == 4 && fee->isVisibleTo(&send),
            "signing failure releases state for retry");
    signing->onSignClicked();
    controller.discardWorkers();
    signing->onValidationResult(validation);
    emit keys.signed_(keys.request, keys.psbt);
    require(controller.workers.size() == 1 && !fee->isVisibleTo(&send),
            "signing answer keeps the transaction locked during finalization");
    crafts = services.transaction.crafts();
    emit feed.defaultFeeRate(1, 9);
    require(send.transaction().feeRate == 7 && services.transaction.crafts() == crafts,
            "pending finalization push leaves transaction unchanged");
    controller.discardWorkers();
    TxResult signedResult{};
    signedResult.is_ok = true;
    signedResult.value = rust::String("00");
    signing->onSignResult(signedResult);
    require(!fee->isVisibleTo(&send), "successful signing retains the preparation lock");
    signing->onCancelClicked();
    require(fee->isVisibleTo(&send) && send.transaction().feeRate == 9,
            "cancelling the signed transaction releases Send and follows the current rate");
    host->removeInstance(instance.id(), &feedPlugin.module);
    host->selectFees({});
}

} // namespace smoke
