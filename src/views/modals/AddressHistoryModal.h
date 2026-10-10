#pragma once

#include "catalog/containers/StepModal.h"
#include <QList>
#include <QPair>
#include <QString>

namespace catalog {
class Button;
class IndexPill;
class LabelRow;
} // namespace catalog

namespace modal {

// The "addresses awaiting payment" modal (design ScreensBasic.jsx
// PrevAddressesModal): a list of past addresses, each with an index pill that
// expands a QR + label editor, plus verify and copy actions.
class AddressHistoryModal : public catalog::StepModal {
    Q_OBJECT

public:
    // `verify_note`, when set, says why an address cannot be (or could not be) verified.
    AddressHistoryModal(const QString &title, const QList<QPair<QString, QString>> &entries,
                        bool verifiable, const QString &verify_note);

signals:
    // Emitted as the modal closes: the verify flow takes its place, and the owner
    // opens the list again once the flow is done.
    void verifyRequested(const QString &address);
    void labelEdited(const QString &address, const QString &label);

public slots:
    void onPillClicked();
    void onVerifyClicked();
    void onLabelChanged(const QString &label);

protected:
    void init();
    void doConnect();
    void view();

private:
    QString m_title;
    QList<QPair<QString, QString>> m_entries;
    bool m_verifiable = true;
    QString m_verify_note;
    QList<catalog::IndexPill *> m_pills;
    QList<catalog::Button *> m_verify_buttons;
    QList<catalog::LabelRow *> m_label_rows;
    QList<QWidget *> m_rows;
    QList<QWidget *> m_expansions;
    int m_open = -1;
};

} // namespace modal
