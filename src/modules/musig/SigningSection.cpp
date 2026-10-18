#include "SigningSection.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/MessageModal.h"
#include "catalog/feedback/Notice.h"
#include "catalog/inputs/ComboBox.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/musig/MusigAccountController.h"
#include "theme/Palette.h"

#include <QComboBox>
#include <QEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <Qontrol>
#include <common.h>
#include <cstddef>
#include <utility>

namespace musig {

using catalog::Label;
using catalog::LabelRole;

namespace {

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// The cosigner gave its share for the PSBT's current round.
auto shareIn(MusigRound round, const MusigCosignerStatus &cosigner) -> bool {
    switch (round) {
    case MusigRound::Nonces:
        return cosigner.nonces >= cosigner.expected;
    case MusigRound::PartialSigs:
        return cosigner.partial_sigs >= cosigner.expected;
    case MusigRound::Ready:
        return true;
    default:
        return false;
    }
}

} // namespace

SigningSection::SigningSection(MusigAccountController *controller) : m_controller(controller) {
    init();
    doConnect();
    view();
    onCoinsSelected({});
}

auto SigningSection::actions() -> QWidget * {
    return m_prepare_btn;
}

void SigningSection::setTransaction(const std::optional<TransactionTemplate> &tx,
                                    const QStringList &outpoints) {
    m_tx = tx;
    // The spend paths follow the coins spent; a running session keeps its own.
    if (outpoints != m_outpoints && m_signing_card->isHidden()) {
        onCoinsSelected(outpoints);
        return;
    }
    updatePrepareState();
}

auto SigningSection::estimatesFee() const -> bool {
    return chosenLeaf() != nullptr;
}

void SigningSection::init() {
    m_leaf_title = new Label(TR("musig-send-spend-path"), LabelRole::Eyebrow);
    m_leaf_combo = new catalog::ComboBox;
    m_leaf_combo->setWidth(Size::XL);

    m_prepare_btn = catalog::btnPrepare();
    m_prepare_btn->setEnabled(false);
    m_prepare_reason_label = new Label(LabelRole::Caption);
    m_prepare_reason_label->setVisible(false);

    initSigningCard();
}

void SigningSection::initSigningCard() {
    m_txid_caption = new Label(TR("history-txid"), LabelRole::InfoLabel);
    m_txid_label = new Label(LabelRole::Mono);
    m_txid_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_round_label = new Label(LabelRole::Subheading);
    m_cosigners_column = new qontrol::Column;
    m_cosigners_column->spacing(resolve(Spacing::XS));
    m_notice = new catalog::Notice;
    m_notice->setVisible(false);
    m_cancel_btn = catalog::btnCancelSigned();
    m_sign_btn = catalog::btnSign();
    m_broadcast_btn = catalog::btnBroadcast();
    m_broadcast_btn->setVisible(false);

    auto *txidRow = (new qontrol::Row)
                        ->spacing(resolve(Spacing::XS))
                        ->push(m_txid_caption)
                        ->push(m_txid_label)
                        ->pushSpacer();
    auto *buttons = (new qontrol::Row)
                        ->spacing(resolve(Spacing::S))
                        ->push(m_cancel_btn)
                        ->pushSpacer()
                        ->push(m_sign_btn)
                        ->push(m_broadcast_btn);
    auto *content = (new qontrol::Column)
                        ->spacing(resolve(Spacing::S))
                        ->push(txidRow)
                        ->push(m_round_label)
                        ->push(m_cosigners_column)
                        ->push(m_notice)
                        ->push(buttons);

    m_signing_card = new catalog::Card;
    m_signing_card->setContent(content);
    m_signing_card->setVisible(false);
}

void SigningSection::doConnect() {
    connect(m_leaf_combo, &QComboBox::currentIndexChanged, this, &SigningSection::onSpendLeafChosen,
            qontrol::UNIQUE);
    connect(m_prepare_btn, &QPushButton::clicked, this, &SigningSection::onPrepareClicked,
            qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &SigningSection::onCancelClicked,
            qontrol::UNIQUE);
    connect(m_sign_btn, &QPushButton::clicked, this, &SigningSection::onSignRoundClicked,
            qontrol::UNIQUE);
    connect(m_broadcast_btn, &QPushButton::clicked, this, &SigningSection::onBroadcastClicked,
            qontrol::UNIQUE);

    connect(m_controller, &MusigAccountController::prepared, this, &SigningSection::onPrepared,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::prepareFailed, this,
            &SigningSection::onPrepareFailed, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::roundChanged, this,
            &SigningSection::onRoundChanged, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::signingFailed, this,
            &SigningSection::onSigningFailed, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::signingCancelled, this,
            &SigningSection::onSigningCancelled, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::transactionReady, this,
            &SigningSection::onTransactionReady, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::finalizeFailed, this,
            &SigningSection::onFinalizeFailed, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::broadcastDone, this,
            &SigningSection::onBroadcastDone, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::updateCoins, this,
            &SigningSection::onCoinsUpdated, qontrol::UNIQUE);
}

void SigningSection::view() {
    auto *col = (new qontrol::Column)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(m_leaf_title)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(m_leaf_combo)
                    ->pushSpacer(resolve(Spacing::XS))
                    ->push(m_prepare_reason_label)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(m_signing_card);
    col->layout()->setContentsMargins(0, 0, 0, 0);
    setLayout(col->layout());
}

void SigningSection::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    account::SendSigning::changeEvent(event);
}

void SigningSection::retranslateUi() {
    m_leaf_title->setText(TR("musig-send-spend-path"));
    auto keys = keyEntries();
    for (std::size_t i = 0; i < m_leaves.leaves.size(); ++i) {
        m_leaf_combo->setItemText(static_cast<int>(i), leafText(m_leaves.leaves[i], keys));
    }
    m_prepare_btn->setText(TR("musig-send-prepare"));
    m_txid_caption->setText(TR("history-txid"));
    m_cancel_btn->setText(TR("common-cancel"));
    m_broadcast_btn->setText(TR("common-broadcast"));
    m_notice->setTitle(failureTitle());
    updatePrepareState();
    if (!m_signing_card->isHidden()) {
        onRoundChanged(m_controller->roundStatus());
    }
}

auto SigningSection::failureTitle() const -> QString {
    switch (m_failure) {
    case Failure::SessionLost:
        return TR("musig-send-session-lost");
    case Failure::Finalize:
        return TR("musig-send-finalize-failed");
    case Failure::Sign:
        return {};
    }
    return {};
}

auto SigningSection::keyEntries() const -> QHash<QString, KeyEntry> {
    QHash<QString, KeyEntry> entries;
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return entries;
    }
    for (const auto &entry : keys->keys(m_controller->network())) {
        entries.insert(entry.fingerprint.toLower(), entry);
    }
    return entries;
}

auto SigningSection::leafText(const MusigSpendLeaf &leaf, const QHash<QString, KeyEntry> &keys)
    -> QString {
    QStringList names;
    for (const auto &fingerprint : leaf.fingerprints) {
        auto fp = toQString(fingerprint);
        auto entry = keys.value(fp.toLower());
        names.append(entry.alias.isEmpty() ? fp : entry.alias);
    }
    QStringList parts;
    if (leaf.key_path) {
        parts.append(TR("musig-send-key-path"));
    }
    parts.append(names.join(QStringLiteral(" + ")));
    if (leaf.relative != 0) {
        parts.append(TR("musig-send-after-blocks").arg(leaf.relative));
    }
    if (leaf.absolute != 0) {
        parts.append(TR("musig-send-from-height").arg(leaf.absolute));
    }
    if (!leaf.spendable) {
        parts.append(TR("musig-send-locked"));
    }
    return parts.join(QStringLiteral(" "));
}

// The first spendable leaf every key of which signs here, else the first spendable one.
auto SigningSection::defaultLeafIndex(const QHash<QString, KeyEntry> &keys) const -> int {
    int firstSpendable = -1;
    for (std::size_t i = 0; i < m_leaves.leaves.size(); ++i) {
        const auto &leaf = m_leaves.leaves[i];
        if (!leaf.spendable) {
            continue;
        }
        if (firstSpendable < 0) {
            firstSpendable = static_cast<int>(i);
        }
        bool allSign = true;
        for (const auto &fingerprint : leaf.fingerprints) {
            if (!keys.value(toQString(fingerprint).toLower()).can_sign) {
                allSign = false;
                break;
            }
        }
        if (allSign) {
            return static_cast<int>(i);
        }
    }
    return firstSpendable < 0 ? 0 : firstSpendable;
}

auto SigningSection::chosenLeaf() const -> const MusigSpendLeaf * {
    if (m_leaf_index < 0 || std::cmp_greater_equal(m_leaf_index, m_leaves.leaves.size())) {
        return nullptr;
    }
    return &m_leaves.leaves[m_leaf_index];
}

void SigningSection::updatePrepareState() {
    // A signing session or a transaction ready to broadcast holds the PSBT in flight.
    if (!m_signing_card->isHidden()) {
        m_prepare_btn->setEnabled(false);
        m_prepare_reason_label->setVisible(false);
        return;
    }
    const auto *leaf = chosenLeaf();
    QString reason;
    if (m_controller->getCoins().empty()) {
        reason = TR("musig-send-no-coins");
    } else if (!m_tx.has_value()) {
        reason = TR("send-invalid-transaction-template");
    } else if (!m_leaves.is_ok) {
        reason = toQString(m_leaves.error);
    } else if (leaf == nullptr) {
        reason = TR("musig-send-no-path");
    } else if (!leaf->spendable) {
        reason = TR("musig-send-path-locked");
    }
    m_prepare_btn->setEnabled(reason.isEmpty());
    m_prepare_reason_label->setText(reason);
    m_prepare_reason_label->setVisible(!reason.isEmpty());
}

void SigningSection::closeSession() {
    m_notice->setVisible(false);
    m_signing_card->setVisible(false);
    m_leaf_combo->setEnabled(true);
    emit lockedChanged(false);
    updatePrepareState();
}

// ===== Slots =====

void SigningSection::onCoinsSelected(const QStringList &outpoints) {
    m_outpoints = outpoints;
    auto inputs = outpoints;
    if (inputs.isEmpty()) {
        for (const auto &coin : m_controller->getCoins()) {
            inputs.append(toQString(coin.outpoint));
        }
    }
    // Keep the path the user picked when the new list still has it.
    const auto *chosen = chosenLeaf();
    bool hadChoice = chosen != nullptr;
    QString previous;
    if (hadChoice) {
        previous = chosen->key_path ? QString() : toQString(chosen->leaf_hash);
    }

    m_leaves = m_controller->spendLeaves(inputs);
    auto keys = keyEntries();
    int index = defaultLeafIndex(keys);
    {
        // The picked index is applied once, below.
        QSignalBlocker blocker(m_leaf_combo);
        m_leaf_combo->clear();
        for (std::size_t i = 0; i < m_leaves.leaves.size(); ++i) {
            const auto &leaf = m_leaves.leaves[i];
            m_leaf_combo->addItem(leafText(leaf, keys));
            auto hash = leaf.key_path ? QString() : toQString(leaf.leaf_hash);
            if (hadChoice && hash == previous) {
                index = static_cast<int>(i);
            }
        }
        if (m_leaves.leaves.empty()) {
            index = -1;
        }
        m_leaf_combo->setCurrentIndex(index);
    }
    onSpendLeafChosen(index);
}

void SigningSection::onCoinsUpdated([[maybe_unused]] CoinState state) {
    // The spend path of a running session never changes.
    if (m_signing_card->isHidden()) {
        onCoinsSelected(m_outpoints);
    }
}

void SigningSection::onSpendLeafChosen(int index) {
    m_leaf_index = index;
    updatePrepareState();
    bool estimates = estimatesFee();
    if (estimates != m_estimates) {
        m_estimates = estimates;
        emit estimateChanged();
    }
}

void SigningSection::onPrepareClicked() {
    const auto *leaf = chosenLeaf();
    if (!m_tx.has_value() || leaf == nullptr) {
        return;
    }
    m_controller->prepareTransaction(m_tx.value(), *leaf);
}

void SigningSection::onPrepared([[maybe_unused]] const QString &psbt_v2_hex, const QString &txid) {
    m_txid_label->setText(txid);
    m_notice->setVisible(false);
    m_sign_btn->setVisible(true);
    m_sign_btn->setEnabled(true);
    m_broadcast_btn->setVisible(false);
    m_cancel_btn->setEnabled(true);
    m_leaf_combo->setEnabled(false);
    m_signing_card->setVisible(true);
    updatePrepareState();
    emit lockedChanged(true);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void SigningSection::onPrepareFailed(const QString &message) {
    AppController::execModal(new catalog::MessageModal(TR("send-prepare-failed"), message));
}

void SigningSection::onRoundChanged(const MusigRoundStatus &status) {
    for (auto *row : m_cosigner_rows) {
        delete m_cosigners_column->take(row);
    }
    m_cosigner_rows.clear();
    // Once finalized only Broadcast is left, a single-key leaf included.
    bool finalized = !m_controller->finalizedTx().isEmpty();
    m_broadcast_btn->setVisible(finalized);
    if (!status.is_ok) {
        m_round_label->setText(toQString(status.error));
        m_sign_btn->setVisible(false);
        return;
    }
    switch (status.round) {
    case MusigRound::Nonces:
        m_round_label->setText(TR("musig-send-round-1"));
        m_sign_btn->setText(TR("musig-send-sign-round").arg(1));
        break;
    case MusigRound::PartialSigs:
        m_round_label->setText(TR("musig-send-round-2"));
        m_sign_btn->setText(TR("musig-send-sign-round").arg(2));
        break;
    case MusigRound::Ready:
        m_round_label->setText(TR("musig-send-ready"));
        break;
    case MusigRound::NotMusig:
        m_round_label->setText(TR("musig-send-single-key"));
        m_sign_btn->setText(TR("btn-sign"));
        break;
    default:
        break;
    }
    m_sign_btn->setVisible(!finalized && status.round != MusigRound::Ready);

    auto keys = keyEntries();
    for (const auto &cosigner : status.cosigners) {
        auto fp = toQString(cosigner.fingerprint);
        auto entry = keys.value(fp.toLower());
        auto *row =
            (new qontrol::Row)
                ->spacing(resolve(Spacing::M))
                ->push(new Label(entry.alias.isEmpty() ? fp : entry.alias))
                ->push(
                    new Label(TR("musig-send-nonces").arg(cosigner.nonces).arg(cosigner.expected),
                              LabelRole::Caption))
                ->push(new Label(
                    TR("musig-send-partial-sigs").arg(cosigner.partial_sigs).arg(cosigner.expected),
                    LabelRole::Caption))
                ->pushSpacer();
        // Hidden, the pill keeps its size so every row has the same height.
        auto *pill = catalog::pillSignerSigned();
        auto policy = pill->sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        pill->setSizePolicy(policy);
        pill->setVisible(shareIn(status.round, cosigner));
        row->push(pill);
        m_cosigners_column->push(row);
        m_cosigner_rows.append(row);
    }
}

void SigningSection::onSignRoundClicked() {
    m_notice->setVisible(false);
    m_controller->signRound();
}

void SigningSection::onSigningFailed(const QString &message, plugin::SignError kind) {
    if (kind == plugin::SignError::MusigSession) {
        m_failure = Failure::SessionLost;
        m_notice->setTone(catalog::Notice::Tone::Warning);
    } else {
        m_failure = Failure::Sign;
        m_notice->setTone(catalog::Notice::Tone::Error);
    }
    m_notice->setTitle(failureTitle());
    m_notice->setText(message);
    m_notice->setVisible(true);
}

void SigningSection::onSigningCancelled() {
    m_notice->setVisible(false);
}

void SigningSection::onTransactionReady([[maybe_unused]] const QString &tx_hex) {
    m_sign_btn->setVisible(false);
    m_broadcast_btn->setVisible(true);
    m_broadcast_btn->setEnabled(true);
}

// Finalizing is pure computation over the PSBT: retrying gives the same error.
void SigningSection::onFinalizeFailed(const QString &message) {
    m_failure = Failure::Finalize;
    m_notice->setTone(catalog::Notice::Tone::Error);
    m_notice->setTitle(failureTitle());
    m_notice->setText(message);
    m_notice->setVisible(true);
}

void SigningSection::onBroadcastClicked() {
    m_broadcast_btn->setEnabled(false);
    m_cancel_btn->setEnabled(false);
    m_controller->broadcast();
}

void SigningSection::onBroadcastDone(const TxResult &result) {
    m_broadcast_btn->setEnabled(true);
    m_cancel_btn->setEnabled(true);
    if (!result.is_ok) {
        AppController::execModal(new catalog::MessageModal(
            TR("common-error"), TR("confirm-send-failed").arg(toQString(result.error))));
        return;
    }
    AppController::execModal(
        new catalog::MessageModal(TR("confirm-transaction-title"), TR("confirm-send-success"),
                                  catalog::MessageModal::Tone::Success, toQString(result.value)));
    closeSession();
    emit sent();
}

void SigningSection::onCancelClicked() {
    m_controller->cancelSigning();
    closeSession();
}

} // namespace musig
