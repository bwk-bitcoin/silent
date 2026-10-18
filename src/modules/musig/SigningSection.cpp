#include "SigningSection.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/MessageModal.h"
#include "catalog/inputs/ComboBox.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/SignModal.h"
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

} // namespace

SigningSection::SigningSection(MusigAccountController *controller) : m_controller(controller) {
    init();
    doConnect();
    view();
    onCoinsSelected({});
}

auto SigningSection::actions() -> QWidget * {
    return m_actions;
}

auto SigningSection::coinsWidget() -> QWidget * {
    return m_leaf_widget;
}

void SigningSection::setTransaction(const std::optional<TransactionTemplate> &tx,
                                    const QStringList &outpoints) {
    m_tx = tx;
    // The spend paths follow the coins spent; a running session keeps its own.
    if (outpoints != m_outpoints && !m_session) {
        onCoinsSelected(outpoints);
        return;
    }
    updateSignState();
}

auto SigningSection::estimatesFee() const -> bool {
    return chosenLeaf() != nullptr;
}

void SigningSection::init() {
    m_leaf_title = new Label(TR("musig-send-spend-path"), LabelRole::Eyebrow);
    m_leaf_combo = new catalog::ComboBox;
    m_leaf_combo->setWidth(Size::XL);
    m_leaf_widget =
        (new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_leaf_title)->push(m_leaf_combo);

    m_reason_label = new Label(LabelRole::Caption);
    m_reason_label->setVisible(false);

    m_cancel_btn = catalog::btnCancelSigned();
    m_cancel_btn->setVisible(false);
    m_sign_btn = catalog::btnSign();
    m_sign_btn->setEnabled(false);
    m_actions =
        (new qontrol::Row)->spacing(resolve(Spacing::S))->push(m_cancel_btn)->push(m_sign_btn);
}

void SigningSection::doConnect() {
    connect(m_leaf_combo, &QComboBox::currentIndexChanged, this, &SigningSection::onSpendLeafChosen,
            qontrol::UNIQUE);
    connect(m_sign_btn, &QPushButton::clicked, this, &SigningSection::onSignClicked,
            qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &SigningSection::onCancelClicked,
            qontrol::UNIQUE);

    connect(m_controller, &MusigAccountController::prepared, this, &SigningSection::onPrepared,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::prepareFailed, this,
            &SigningSection::onPrepareFailed, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::broadcastDone, this,
            &SigningSection::onBroadcastDone, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::updateCoins, this,
            &SigningSection::onCoinsUpdated, qontrol::UNIQUE);
}

void SigningSection::view() {
    // The step's own widget, under the fee row: why Sign cannot start yet.
    auto *col = (new qontrol::Column)->push(m_reason_label);
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
    m_sign_btn->setText(TR("btn-sign"));
    m_cancel_btn->setText(TR("common-cancel"));
    updateSignState();
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

void SigningSection::updateSignState() {
    // While a session runs, Sign opens its modal again and Cancel drops it.
    m_cancel_btn->setVisible(m_session);
    if (m_session) {
        m_sign_btn->setEnabled(true);
        m_reason_label->setVisible(false);
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
    m_sign_btn->setEnabled(reason.isEmpty());
    m_reason_label->setText(reason);
    m_reason_label->setVisible(!reason.isEmpty());
}

void SigningSection::openSignModal() {
    m_sent = false;
    // Returns once the modal has closed; onBroadcastDone() sets m_sent meanwhile.
    AppController::execModal(new SignModal(m_controller));
    if (!m_sent) {
        return;
    }
    AppController::execModal(new catalog::MessageModal(
        TR("send-sent-title"), TR("send-sent-message"), catalog::MessageModal::Tone::Success));
    closeSession();
    emit sent();
}

void SigningSection::closeSession() {
    m_session = false;
    m_leaf_combo->setEnabled(true);
    emit lockedChanged(false);
    updateSignState();
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
    if (!m_session) {
        onCoinsSelected(m_outpoints);
    }
}

void SigningSection::onSpendLeafChosen(int index) {
    m_leaf_index = index;
    updateSignState();
    bool estimates = estimatesFee();
    if (estimates != m_estimates) {
        m_estimates = estimates;
        emit estimateChanged();
    }
}

void SigningSection::onSignClicked() {
    if (m_session) {
        openSignModal();
        return;
    }
    const auto *leaf = chosenLeaf();
    if (!m_tx.has_value() || leaf == nullptr) {
        return;
    }
    // Starts the session: onPrepared() opens the Sign modal.
    m_controller->prepareTransaction(m_tx.value(), *leaf);
}

void SigningSection::onPrepared([[maybe_unused]] const QString &psbt_v2_hex,
                                [[maybe_unused]] const QString &txid) {
    m_session = true;
    m_leaf_combo->setEnabled(false);
    updateSignState();
    emit lockedChanged(true);
    openSignModal();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void SigningSection::onPrepareFailed(const QString &message) {
    AppController::execModal(new catalog::MessageModal(TR("send-prepare-failed"), message));
}

void SigningSection::onBroadcastDone(const TxResult &result) {
    // The Sign modal shows a failure itself.
    m_sent = result.is_ok;
}

void SigningSection::onCancelClicked() {
    m_controller->cancelSigning();
    closeSession();
}

} // namespace musig
