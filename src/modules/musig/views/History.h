#pragma once

#include <QEvent>
#include <QWidget>
#include <Qontrol>
#include <silent.h>

class MusigAccountController;

namespace catalog {
class BalanceHeader;
class HistoryTable;
} // namespace catalog

namespace musig::view {

class History : public qontrol::Screen {
    Q_OBJECT

public:
    explicit History(MusigAccountController *ctrl);

public slots:
    void onCoinsUpdated(CoinState state);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;

private:
    MusigAccountController *m_controller = nullptr;
    CoinState m_state{};
    QWidget *m_main_widget = nullptr;
    catalog::BalanceHeader *m_balance = nullptr;
    catalog::HistoryTable *m_table = nullptr;
};

} // namespace musig::view
