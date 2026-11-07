// The shared account views over the built-in service adapters of a scripted account: what
// the account answers, and when, is the test's. Covers reads by polling and by notification
// around a funds read that answers later, coin selection answered inline, later and stale,
// independent templates of two Send views, a label kept in RAM when its save fails, and the
// broadcast outcomes the SP signing step shows.

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <Qontrol>
#include <common.h>
#include <memory>
#include <optional>
#include <utility>

#define private public
#define protected public
#include "account/SendSigning.h"
#include "account/Services.h"
#include "account/services/ServiceAdapters.h"
#include "account/services/ServiceContext.h"
#include "account/views/Coins.h"
#include "account/views/History.h"
#include "account/views/Receive.h"
#include "account/views/Send.h"
#include "account/views/Settings.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/EndpointRow.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/Input.h"
#include "i18n/I18nManager.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"
#include "modules/sp/AccountController.h"
#include "modules/sp/SpSigning.h"
#undef private
#undef protected

namespace smoke {
namespace {

constexpr int TIMEOUT_MS = 10000;
constexpr quint64 FEE = 200;
constexpr quint64 COIN_A = 100000;
constexpr quint64 COIN_B = 50000;

const QString TX_A = QString(64, QLatin1Char('a'));
const QString TX_B = QString(64, QLatin1Char('b'));
const QString OUTPOINT_A = TX_A + QStringLiteral(":0");
const QString OUTPOINT_B = TX_B + QStringLiteral(":1");
const QString RECIPIENT = QStringLiteral("bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080");
const QString LABEL_FAILURE = QStringLiteral("Label is not saved: disk full");

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("account views: %s", message);
    }
}

template <typename Predicate> void waitFor(Predicate done, const char *message) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(done(), message);
}

// One account, scripted. Funds answer at once unless delayed; a coin selection answers
// at once or once the test answers it; labels save, or stay in RAM with error().
class ViewContext final : public account::ServiceContext {
    Q_OBJECT

public:
    enum class Answer { Inline, Later };

    Funds funds{.coins = {{.outpoint = OUTPOINT_A,
                           .amount = COIN_A,
                           .height = 120,
                           .origin = QStringLiteral("SP"),
                           .spendable = true},
                          {.outpoint = OUTPOINT_B,
                           .amount = COIN_B,
                           .origin = QStringLiteral("SP"),
                           .spendable = true}},
                .balance = {.confirmed = COIN_A,
                            .unconfirmed = COIN_B,
                            .confirmedCount = 1,
                            .unconfirmedCount = 1},
                .history = {{.txid = TX_A,
                             .direction = QStringLiteral("incoming"),
                             .status = QStringLiteral("verified"),
                             .amount = COIN_A,
                             .height = 120,
                             .timestamp = 1700000000},
                            {.txid = TX_B,
                             .direction = QStringLiteral("incoming"),
                             .status = QStringLiteral("unconfirmed"),
                             .amount = COIN_B}}};
    QList<backend::Label> savedLabels;
    bool delayFunds = false;
    int fundsReads = 0;
    Answer selection = Answer::Inline;
    QList<std::pair<backend::RequestId, backend::TransactionTemplate>> selections;
    bool labelPersists = true;
    // finalize() builds nothing, for this reason.
    QString finalizeFailure;
    int tipRequests = 0;
    int capabilityRequests = 0;
    Receiving receiving;
    backend::BroadcastResult broadcastResult;
    QList<QByteArray> broadcasts;

    ViewContext() : ServiceContext(QStringLiteral("views"), nullptr) {
        publishNetwork(backend::Network::Regtest);
    }

    // Complete over the given inputs: a MAX output takes what the fixed ones and the fee
    // leave.
    auto craft(const backend::TransactionTemplate &transaction) -> backend::CraftResult override {
        if (transaction.inputs.isEmpty()) {
            return {.transaction = transaction,
                    .completion = {.error = QStringLiteral("No inputs are selected")}};
        }
        quint64 total = 0;
        for (const auto &coin : funds.coins) {
            total += transaction.inputs.contains(coin.outpoint) ? coin.amount : 0;
        }
        auto crafted = transaction;
        quint64 fixed = FEE;
        for (const auto &output : crafted.outputs) {
            fixed += output.maximum ? 0 : output.amount;
        }
        for (auto &output : crafted.outputs) {
            if (output.maximum) {
                output.amount = total > fixed ? total - fixed : 0;
            }
        }
        crafted.fee = FEE;
        return {.transaction = crafted, .completion = {.complete = total >= fixed}};
    }

    auto finalize(const backend::TransactionTemplate &transaction)
        -> std::optional<QByteArray> override {
        if (!finalizeFailure.isEmpty()) {
            emit finalizeFailed(finalizeFailure);
            return std::nullopt;
        }
        return QByteArrayLiteral("psbt:") + transaction.inputs.join(',').toLatin1();
    }

    void setState(const backend::BlockchainState &state) {
        publishState(state);
    }

    void setSettings(const backend::Settings &settings) {
        publishSettings(settings);
    }

    void selectCoins(backend::RequestId token, backend::TransactionTemplate transaction) override {
        selections.append({token, transaction});
        if (selection == Answer::Inline) {
            answerSelection(selections.size() - 1);
        }
    }

    // Selection takes every coin.
    void answerSelection(qsizetype index) {
        auto [token, transaction] = selections.at(index);
        transaction.inputs = {OUTPOINT_A, OUTPOINT_B};
        emit coinsSelected(token, craft(transaction));
    }

    void answerFunds() {
        fundsRead(funds);
    }

    void newAddress(backend::RequestId token, [[maybe_unused]] QString kind) override {
        emit failed(token, unsupported());
    }

    // The RAM edit shows first, as a store keeps it when its save fails.
    void setLabel(backend::RequestId token, backend::LabelKey key, QString text) override {
        savedLabels.removeIf([&key](const backend::Label &label) {
            return label.key.kind == key.kind && label.key.id == key.id;
        });
        if (!text.isEmpty()) {
            savedLabels.append({.key = key, .text = text});
        }
        for (auto &coin : funds.coins) {
            if (key.kind == backend::LabelKind::Outpoint && coin.outpoint == key.id) {
                coin.label = text;
            }
        }
        onLabelsChanged();
        onFundsChanged();
        if (labelPersists) {
            emit labelSaved(token);
        } else {
            emit failed(token, LABEL_FAILURE);
        }
    }

    void fetchTransactionDetails(backend::RequestId token, [[maybe_unused]] QString txid) override {
        emit failed(token, unsupported());
    }

    void connectAll(backend::RequestId token, [[maybe_unused]] bool start) override {
        emit failed(token, unsupported());
    }

    void setConnected(backend::RequestId token, [[maybe_unused]] QString connection,
                      [[maybe_unused]] bool connected) override {
        emit failed(token, unsupported());
    }

    void refreshTip(backend::RequestId token, [[maybe_unused]] QString connection) override {
        ++tipRequests;
        emit failed(token, unsupported());
    }

    void testEndpoint(backend::RequestId token,
                      [[maybe_unused]] backend::Endpoint endpoint) override {
        emit failed(token, unsupported());
    }

    void saveSettings(backend::RequestId token,
                      [[maybe_unused]] backend::Settings settings) override {
        emit failed(token, unsupported());
    }

    void endpointCapabilities(backend::RequestId token,
                              [[maybe_unused]] QString endpoint) override {
        ++capabilityRequests;
        emit failed(token, unsupported());
    }

    void broadcast(backend::RequestId token, QByteArray transaction) override {
        broadcasts.append(transaction);
        emit broadcastFinished(token, broadcastResult);
    }

protected:
    auto startFundsRead() -> bool override {
        ++fundsReads;
        if (!delayFunds) {
            fundsRead(funds);
        }
        return true;
    }

    auto readLabels() -> std::optional<QList<backend::Label>> override {
        return savedLabels;
    }

    auto readReceiving() -> std::optional<Receiving> override {
        return receiving;
    }

    static auto unsupported() -> QString {
        return QStringLiteral("Not scripted");
    }
};

// Every service of `context`, as the built-in providers supply them.
class ViewServices {
public:
    explicit ViewServices(ViewContext *context) {
        auto *parent = &m_owner;
        services = {.account = qobject_cast<backend::IAccountBackend *>(
                        account::serviceAdapter(backend::ServiceKind::Account, context, parent)),
                    .blockchain = qobject_cast<backend::IBlockchain *>(
                        account::serviceAdapter(backend::ServiceKind::Blockchain, context, parent)),
                    .transaction = qobject_cast<backend::ITransaction *>(account::serviceAdapter(
                        backend::ServiceKind::Transaction, context, parent)),
                    .labels = qobject_cast<backend::ILabels *>(
                        account::serviceAdapter(backend::ServiceKind::Labels, context, parent)),
                    .settings = qobject_cast<backend::IBackendSettings *>(
                        account::serviceAdapter(backend::ServiceKind::Settings, context, parent))};
    }

    account::Services services;

private:
    QObject m_owner;
};

// The signing step, recording what Send hands it.
class RecordingSigning final : public account::SendSigning {
    Q_OBJECT

public:
    QList<std::optional<backend::TransactionTemplate>> transactions;
    QList<QStringList> outpoints;

    // Send lays it out and owns it from there.
    auto actions() -> QWidget * override {
        return m_actions;
    }

    auto coinsWidget() -> QWidget * override {
        return nullptr;
    }

    void setTransaction(const std::optional<backend::TransactionTemplate> &tx,
                        const QStringList &spent) override {
        transactions.append(tx);
        outpoints.append(spent);
    }

    [[nodiscard]] auto estimatesFee() const -> bool override {
        return true;
    }

private:
    QWidget *m_actions = new QWidget;
};

class Answers final : public QObject {
    Q_OBJECT

public:
    int coinsChanged = 0;

public slots:

    void onCoinsChanged() {
        ++coinsChanged;
    }
};

// Closes the message modal the signing step shows, keeping its title and texts.
class MessageCatcher final : public QObject {
    Q_OBJECT

public:
    QStringList titles;
    QStringList texts;

public slots:

    void onTimeout() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        titles.append(dialog->windowTitle());
        QStringList shown;
        for (auto *label : dialog->findChildren<QLabel *>()) {
            shown.append(label->text());
        }
        texts.append(shown.join(QLatin1Char('\n')));
        dialog->done(QDialog::Accepted);
    }
};

class SentSpy final : public QObject {
    Q_OBJECT

public:
    int sent = 0;

public slots:

    void onSent() {
        ++sent;
    }
};

// An SP controller with no account: what the signing step needs of it around a broadcast.
class IdleController final : public AccountController {
public:
    IdleController() : AccountController(static_cast<AccountWidget *>(nullptr)) {
    }
};

auto amountInput(account::view::Send &send) -> QLineEdit * {
    for (auto *input : send.findChildren<catalog::Input *>()) {
        if (input->placeholderText() == TR("send-placeholder-amount")) {
            return input;
        }
    }
    qFatal("account views: no amount input");
}

auto addressInput(account::view::Send &send) -> QLineEdit * {
    for (auto *input : send.findChildren<catalog::Input *>()) {
        if (input->placeholderText() == TR("send-placeholder-address")) {
            return input;
        }
    }
    qFatal("account views: no address input");
}

// The recipient's amount Send last handed the signing step, nullopt for no transaction.
auto signedAmount(const RecordingSigning &signing) -> std::optional<quint64> {
    if (signing.transactions.isEmpty() || !signing.transactions.last().has_value()) {
        return std::nullopt;
    }
    return signing.transactions.last()->outputs.first().amount;
}

void pay(account::view::Send &send, const QString &btc) {
    addressInput(send)->setText(RECIPIENT);
    amountInput(send)->setText(btc);
}

// The coins and history reach a polling consumer, a notified one and the views from the
// same read, once it answers; changes during a read are read again after it.
void readsAroundALaterFundsRead() {
    ViewContext context;
    context.delayFunds = true;
    ViewServices view(&context);
    Answers answers;
    QObject::connect(view.services.blockchain, &backend::IBlockchain::coinsChanged, &answers,
                     &Answers::onCoinsChanged, qontrol::UNIQUE);
    account::view::Coins coins(view.services);
    account::view::History history(view.services);

    context.onFundsChanged();
    context.onFundsChanged();
    require(context.fundsReads == 1, "one funds read runs at a time");
    require(view.services.blockchain->coins().isEmpty() && coins.m_coins.isEmpty() &&
                history.m_history.isEmpty() && answers.coinsChanged == 0,
            "nothing is published before the read answers");

    context.answerFunds();
    require(context.fundsReads == 2, "a change during the read is read again after it");
    require(view.services.blockchain->coins().size() == 2,
            "a polling consumer reads the answered coins");
    require(answers.coinsChanged == 1, "a notified consumer is told once");
    require(coins.m_coins.size() == 2 && coins.m_coins.first().outpoint == OUTPOINT_B,
            "Coins shows both coins, the unconfirmed one first");
    require(coins.m_dates.value(TX_A) == 1700000000 && !coins.m_dates.contains(TX_B),
            "a coin is dated by its funding transaction");
    require(coins.m_balance.confirmed == COIN_A && coins.m_balance.unconfirmed == COIN_B,
            "Coins shows the balance");
    require(history.m_history.size() == 2 && history.m_history.first().txid == TX_B,
            "History shows both transactions, the unconfirmed one first");
    context.answerFunds();
    require(answers.coinsChanged == 1, "an unchanged read notifies nothing");
}

// Coin selection answered during the request, later, and stale: only the answer to the
// latest template is shown, and the signing step holds nothing while it waits.
void sendSelection() {
    ViewContext context;
    ViewServices view(&context);
    context.onFundsChanged();
    auto *signing = new RecordingSigning;
    account::view::Send send(view.services, signing);

    pay(send, QStringLiteral("0.0001"));
    require(!context.selections.isEmpty() && context.selections.last().second.inputs.isEmpty(),
            "auto selection asks the account to select, with no input");
    require(send.m_preview.has_value() && signedAmount(*signing) == 10000,
            "an inline answer is shown during the request");
    require(send.m_preview->transaction.inputs == QStringList({OUTPOINT_A, OUTPOINT_B}) &&
                send.m_preview->transaction.fee == FEE &&
                send.m_preview->amounts.change == COIN_A + COIN_B - 10000 - FEE,
            "the preview is the crafted transaction, with its fee and change");
    require(send.m_auto_selected_outpoints == QStringList({OUTPOINT_A, OUTPOINT_B}),
            "the selected coins are marked");

    context.selection = ViewContext::Answer::Later;
    amountInput(send)->setText(QStringLiteral("0.0002"));
    require(!send.m_preview.has_value() && !signedAmount(*signing).has_value(),
            "nothing is signed while the selection is pending");
    require(signing->outpoints.last() == QStringList({OUTPOINT_A, OUTPOINT_B}),
            "the step keeps the coins it had while the selection is pending, and what it "
            "chose for them");
    context.answerSelection(context.selections.size() - 1);
    require(send.m_preview.has_value() && signedAmount(*signing) == 20000,
            "a later answer is shown when it comes");

    amountInput(send)->setText(QStringLiteral("0.0003"));
    amountInput(send)->setText(QStringLiteral("0.0004"));
    auto stale = context.selections.size() - 2;
    context.answerSelection(stale);
    require(!send.m_preview.has_value() && !signedAmount(*signing).has_value(),
            "the answer to an older template is dropped");
    context.answerSelection(stale + 1);
    require(signedAmount(*signing) == 40000, "the answer to the latest template is shown");

    send.m_auto_coin_selection->setChecked(false);
    send.onAutoSelectionToggled();
    auto asked = context.selections.size();
    require(send.m_selected_coins.size() == 2 && signedAmount(*signing) == 40000 &&
                context.selections.size() == asked,
            "coins picked by hand are crafted at once, with no selection");
}

// Two Send views over the same services hold their own templates: answers go to the
// view that asked, in whatever order they come.
void independentTemplates() {
    ViewContext context;
    ViewServices view(&context);
    context.onFundsChanged();
    context.selection = ViewContext::Answer::Later;
    auto *firstSigning = new RecordingSigning;
    auto *secondSigning = new RecordingSigning;
    account::view::Send first(view.services, firstSigning);
    account::view::Send second(view.services, secondSigning);

    pay(first, QStringLiteral("0.0001"));
    pay(second, QStringLiteral("0.0002"));
    auto firstAsk = context.selections.size() - 2;
    require(context.selections.at(firstAsk).first != context.selections.at(firstAsk + 1).first,
            "each view asks with its own request");
    context.answerSelection(firstAsk + 1);
    require(signedAmount(*secondSigning) == 20000 && !signedAmount(*firstSigning).has_value(),
            "the second view's answer reaches only the second view");
    context.answerSelection(firstAsk);
    require(signedAmount(*firstSigning) == 10000 && signedAmount(*secondSigning) == 20000,
            "the first view's later answer reaches only the first view");
}

// A label kept in RAM when its save fails shows in the row, with the failure above it.
void labelFailureShown() {
    ViewContext context;
    ViewServices view(&context);
    context.onFundsChanged();
    account::view::Coins coins(view.services);

    context.labelPersists = false;
    coins.onCoinLabelEdited(OUTPOINT_A, QStringLiteral("rent"));
    require(view.services.labels->label({.kind = backend::LabelKind::Outpoint, .id = OUTPOINT_A}) ==
                QStringLiteral("rent"),
            "the RAM edit is published");
    auto shown = std::ranges::find_if(
        coins.m_coins, [](const backend::Coin &coin) { return coin.outpoint == OUTPOINT_A; });
    require(shown != coins.m_coins.end() && shown->label == QStringLiteral("rent"),
            "the row shows the label kept in RAM");
    require(coins.m_notice->isVisibleTo(&coins) && coins.m_label_error == LABEL_FAILURE,
            "the failed save is shown, not reported saved");

    context.labelPersists = true;
    coins.onCoinLabelEdited(OUTPOINT_A, QStringLiteral("rent"));
    require(!coins.m_notice->isVisibleTo(&coins) && coins.m_label_error.isEmpty(),
            "a save that goes through clears the failure");
}

// The SP signing step broadcasts through IBlockchain: only an accepted transaction is shown
// sent, rejected and unknown fates are failures with their error.
void broadcastOutcomes() {
    struct Case {
        backend::BroadcastOutcome outcome;
        bool sent;
    };

    for (auto [outcome, sent] : {Case{backend::BroadcastOutcome::Accepted, true},
                                 Case{backend::BroadcastOutcome::Rejected, false},
                                 Case{backend::BroadcastOutcome::Unknown, false}}) {
        ViewContext context;
        context.broadcastResult = {.txid = TX_A,
                                   .outcome = outcome,
                                   .error = sent ? QString() : QStringLiteral("server said no"),
                                   .warnings = {QStringLiteral("not recorded locally")}};
        ViewServices view(&context);
        IdleController controller;
        SpSigning signing(&controller, view.services);
        SentSpy spy;
        QObject::connect(&signing, &account::SendSigning::sent, &spy, &SentSpy::onSent,
                         qontrol::UNIQUE);
        signing.m_tx_template = backend::TransactionTemplate{.inputs = {OUTPOINT_A}};
        signing.m_signed_tx_hex = QStringLiteral("0200");
        MessageCatcher catcher;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &catcher, &MessageCatcher::onTimeout,
                         qontrol::UNIQUE);
        timer.start(20);
        signing.onBroadcastClicked();
        waitFor([&catcher] { return !catcher.titles.isEmpty(); }, "the outcome is shown");
        require(context.broadcasts == QList<QByteArray>{QByteArray::fromHex("0200")},
                "the signed transaction is broadcast as its bytes");
        if (sent) {
            require(catcher.titles.first() == TR("send-sent-title") && spy.sent == 1,
                    "an accepted broadcast shows the transaction sent, its warnings aside");
        } else {
            require(catcher.titles.first() == TR("common-error") &&
                        catcher.texts.first().contains(QStringLiteral("server said no")) &&
                        spy.sent == 0,
                    "a rejected or unknown fate is a failure with its error, never sent");
        }
        require(signing.m_signed_tx_hex.isEmpty() && !signing.m_tx_template.has_value(),
                "the step lets go of the transaction either way");
    }
}

// Settings asks the heights and capabilities again when a connection or a saved server
// changes, never on a scan or tip update.
void settingsIgnoresScanUpdates() {
    ViewContext context;
    context.setSettings(
        {.network = backend::Network::Regtest,
         .endpoints = {{.id = QStringLiteral("blindbit"),
                        .name = QStringLiteral("BlindBit"),
                        .url = QStringLiteral("http://127.0.0.1:1")},
                       {.id = QStringLiteral("electrum"), .name = QStringLiteral("Electrum")}}});
    backend::BlockchainState state{
        .scanning = true,
        .progress = {{.id = QStringLiteral("blindbit"), .height = 1, .target = 10}},
        .connections = {{.id = QStringLiteral("blindbit"), .connected = true},
                        {.id = QStringLiteral("electrum")}}};
    context.setState(state);
    ViewServices view(&context);
    account::view::Settings settings(view.services, nullptr);
    settings.show();
    require(settings.row(QStringLiteral("blindbit"))->presentation.capabilities &&
                settings.row(QStringLiteral("electrum"))->ssl != nullptr &&
                settings.row(QStringLiteral("blindbit"))->label->text() ==
                    TR("settings-blindbit-address"),
            "the rows take the provider's presentation");
    auto tips = context.tipRequests;
    auto capabilities = context.capabilityRequests;
    require(tips >= 1 && capabilities >= 1, "a shown Settings asks the height and capabilities");

    state.progress.first().height = 2;
    state.tip = 10;
    context.setState(state);
    require(context.tipRequests == tips && context.capabilityRequests == capabilities,
            "a scan or tip update asks nothing again");

    state.connections.first().error = QStringLiteral("lost");
    context.setState(state);
    require(context.tipRequests == tips + 1 && context.capabilityRequests == capabilities + 1,
            "a connection change asks the height and capabilities again");
    settings.hide();
}

// The kinds' names follow the provider, a language change included; their awaiting
// titles come from its extension.
void receiveFollowsKindNames() {
    ViewContext context;
    context.receiving = {.kinds = {{.id = QStringLiteral("segwit"), .name = QStringLiteral("A")},
                                   {.id = QStringLiteral("taproot"), .name = QStringLiteral("B")}}};
    context.onReceivingChanged();
    ViewServices view(&context);
    account::view::Receive receive(view.services, nullptr);
    require(receive.m_sections.size() == 2 && receive.m_sections.first().fold->m_title == "A",
            "a section is titled by its kind's name");
    require(receive.awaitingTitle(receive.m_sections.first().kind) == TR("receive-awaiting-segwit"),
            "the awaiting title comes from the provider");

    context.receiving.kinds.first().name = QStringLiteral("A2");
    context.onReceivingChanged();
    require(receive.m_sections.first().fold->m_title == "A2", "a renamed kind is shown renamed");
    context.receiving.kinds.first().name = QStringLiteral("A3");
    // The provider names its kinds again in the new language.
    emit i18n::I18nManager::get() -> languageChanged(QStringLiteral("en"), true);
    require(receive.m_sections.first().fold->m_title == "A3",
            "a language change shows the names in the new language");
}

// A finalize() failure shows its reason, as a prepare failure did.
void finalizeReasonShown() {
    ViewContext context;
    context.finalizeFailure = QStringLiteral("no change address");
    ViewServices view(&context);
    IdleController controller;
    SpSigning signing(&controller, view.services);
    signing.m_tx = backend::TransactionTemplate{.inputs = {OUTPOINT_A}};
    MessageCatcher catcher;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &catcher, &MessageCatcher::onTimeout,
                     qontrol::UNIQUE);
    timer.start(20);
    signing.onSignClicked();
    waitFor([&catcher] { return !catcher.titles.isEmpty(); }, "the failure is shown");
    require(catcher.titles.first() == TR("send-prepare-failed") &&
                catcher.texts.first().contains(QStringLiteral("no change address")),
            "the prepare failure says why");
    require(!signing.m_tx_template.has_value(), "nothing is held after a failed prepare");
}

} // namespace

void accountViewsSmoke() {
    readsAroundALaterFundsRead();
    sendSelection();
    independentTemplates();
    labelFailureShown();
    broadcastOutcomes();
    settingsIgnoresScanUpdates();
    receiveFollowsKindNames();
    finalizeReasonShown();
}

} // namespace smoke

#include "account_views_smoke.moc"
