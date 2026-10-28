#include "SpSigningProbe.h"

#include "AppController.h"
#include "PriceProbe.h"
#include "account/views/Send.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
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

    auto transaction() -> TransactionTemplate {
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
    SpSend send(&controller);
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
    require(send.transaction().fee_rate == 0.5 && send.transaction().outputs[0].max,
            "real funded SP Send starts at 0.5 sat/vB with MAX");
    auto simulation = controller.getAccount().value()->simulate_transaction(send.transaction());
    require(simulation.is_valid && simulation.fee > 0, "real funded template constructs");
    auto frozenAmount = amount->text();
    signing->onSignClicked();
    require(controller.workers.size() == 1 && fee->isHidden() && !amount->isEnabled(),
            "successful actual preparation locks before submitting validation");
    auto simulations = controller.simulations;
    emit feed.defaultFeeRate(1, 5);
    require(send.transaction().fee_rate == 0.5 && amount->text() == frozenAmount &&
                controller.simulations == simulations,
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
    require(answer.answers == 1 && !fee->isHidden() && send.transaction().fee_rate == 5,
            "missing keys releases prepared state and follows the cached rate");

    host->setKeys(&keys, &feedPlugin.module);
    signing->onSignClicked();
    controller.discardWorkers();
    signing->onValidationResult(validation);
    require(keys.requests == 1 && !keys.psbt.isEmpty() && fee->isHidden(),
            "retry reaches the actual deferred signing request while locked");
    frozenAmount = amount->text();
    auto prepared = keys.psbt;
    simulations = controller.simulations;
    emit feed.defaultFeeRate(1, 7);
    require(send.transaction().fee_rate == 5 && amount->text() == frozenAmount &&
                controller.simulations == simulations && keys.psbt == prepared &&
                keys.requests == 1,
            "pending signer push cannot change fee MAX simulation or prepared PSBT");
    emit keys.cancelled(keys.request);
    require(!fee->isHidden() && send.transaction().fee_rate == 7,
            "signer cancellation releases state and catches up");

    signing->onSignClicked();
    controller.discardWorkers();
    validation.spent_input_count = 1;
    signing->onValidationResult(validation);
    require(answer.answers == 2 && !fee->isHidden() && keys.requests == 1,
            "spent-input refusal unlocks without starting a signer");
    validation.spent_input_count = 0;
    validation.reused_output_count = 1;
    answer.accept = false;
    signing->onSignClicked();
    controller.discardWorkers();
    signing->onValidationResult(validation);
    require(answer.answers == 3 && !fee->isHidden() && keys.requests == 1,
            "reuse-warning rejection releases the prepared transaction");
    validation.reused_output_count = 0;
    answer.accept = true;

    signing->onSignClicked();
    controller.discardWorkers();
    signing->onValidationResult(validation);
    emit keys.error(keys.request, QStringLiteral("fixture signing failure"));
    require(answer.answers == 4 && !fee->isHidden(), "signing failure releases state for retry");
    signing->onSignClicked();
    controller.discardWorkers();
    signing->onValidationResult(validation);
    emit keys.signed_(keys.request, keys.psbt);
    require(controller.workers.size() == 1 && fee->isHidden(),
            "signing answer keeps the transaction locked during finalization");
    simulations = controller.simulations;
    emit feed.defaultFeeRate(1, 9);
    require(send.transaction().fee_rate == 7 && controller.simulations == simulations,
            "pending finalization push leaves transaction unchanged");
    controller.discardWorkers();
    TxResult signedResult{};
    signedResult.is_ok = true;
    signedResult.value = rust::String("00");
    signing->onSignResult(signedResult);
    require(fee->isHidden(), "successful signing retains the preparation lock");
    signing->onCancelClicked();
    require(!fee->isHidden() && send.transaction().fee_rate == 9,
            "cancelling the signed transaction releases Send and follows the current rate");
    host->removeInstance(instance.id(), &feedPlugin.module);
    host->selectFees({});
}

} // namespace smoke
