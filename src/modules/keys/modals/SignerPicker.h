#pragma once

#include <QIcon>
#include <QList>
#include <QString>

#include <Qontrol>
#include <silent.h>

class ISigner;

namespace catalog {
class Button;
class SelectRow;
} // namespace catalog

namespace keys::modal {

// PSBT-driven signer picker, built from a SigningPlan: one row per fingerprint the
// PSBT needs (registered or not, usable or not), plus a disabled hint section for
// connected devices the PSBT does not need. While an output script is still
// underived (sp_outputs_pending), only the must-sign-first row stays pickable.
class SignerPicker final : public qontrol::Modal {
    Q_OBJECT

public:
    SignerPicker(const QString &title, const SigningPlan &plan, QWidget *parent = nullptr);

    [[nodiscard]] auto selectedFingerprint() const -> QString;

signals:
    void picked(QString fingerprint);

public slots:
    void onRowClicked();

protected:
    void init();
    void doConnect();
    void view();
    [[nodiscard]] auto buildCandidateRow(const SigningCandidate &candidate) const
        -> catalog::SelectRow *;
    static auto buildHintRow(ISigner *signer) -> catalog::SelectRow *;
    [[nodiscard]] static auto reasonText(KeyUnusableReason reason) -> QString;
    [[nodiscard]] static auto routeIcon(KeyRouteKind route) -> QIcon;

private:
    QString m_title;
    QString m_selected_fingerprint;
    QList<SigningCandidate> m_candidates;
    bool m_sp_outputs_pending = false;
    catalog::Button *m_cancel_btn = nullptr;
};

} // namespace keys::modal
