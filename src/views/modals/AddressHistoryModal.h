#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <Qontrol>

namespace catalog {
class IndexPill;
class ScrollArea;
} // namespace catalog

namespace modal {

// The "addresses awaiting payment" modal (design ScreensBasic.jsx
// PrevAddressesModal): a list of past addresses, each with an index pill that
// expands a QR + label editor, plus verify and copy actions.
class AddressHistoryModal : public qontrol::Modal {
    Q_OBJECT

public:
    AddressHistoryModal(const QString &title, const QList<QPair<QString, QString>> &entries,
                        QWidget *parent = nullptr);

signals:
    // Emitted as the modal closes: the verify flow takes its place, and the owner
    // opens the list again once the flow is done.
    void verifyRequested(const QString &address);
    void labelEdited(const QString &address, const QString &label);

public slots:
    void onPillClicked();
    void onVerifyClicked();
    void onLabelChanged(const QString &label);
    // Regrows the dialog to fit the (collapsed/expanded) content. Posted queued
    // so the layout has settled first.
    void onScrollToOpen();
    // Scrolls the open row's top to the top of the scroll area so its expanded
    // QR is revealed. Posted queued after onScrollToOpen so the resize settles.
    void onScrollAlign();

private:
    void build(const QString &title, const QList<QPair<QString, QString>> &entries);

    catalog::ScrollArea *m_scroll = nullptr;
    QList<catalog::IndexPill *> m_pills;
    QList<QWidget *> m_rows;
    QList<QWidget *> m_expansions;
    int m_open = -1;
};

} // namespace modal
