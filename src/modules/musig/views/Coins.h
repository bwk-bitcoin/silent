#pragma once

#include "theme/Palette.h"

#include <QString>
#include <QWidget>
#include <Qontrol>
#include <silent.h>

class MusigAccountController;

namespace catalog {
class BalanceHeader;
class Table;
} // namespace catalog

namespace musig::view {

class Coins : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Coins(MusigAccountController *ctrl);

    static const int TYPE_W = resolve(Size::XS);
    static const int HEIGHT_W = resolve(Size::XS);
    static const int OUTPOINT_W = resolve(Size::S);
    static const int LABEL_W = resolve(Size::M);
    static const int VALUE_W = resolve(Size::S);

public slots:
    void onCoinsUpdated(CoinState state);
    void onCellEdited(const QString &outpoint, int column, const QString &text);
    void onLabelEdited(const QString &outpoint, const QString &label);

protected:
    void init() override;
    void doConnect() override;
    void view() override;

private:
    MusigAccountController *m_controller = nullptr;
    CoinState m_state{};
    QWidget *m_main_widget = nullptr;
    catalog::BalanceHeader *m_balance = nullptr;
    catalog::Table *m_table = nullptr;
};

} // namespace musig::view
