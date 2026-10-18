#pragma once

#include <QEvent>
#include <QWidget>
#include <Qontrol>
#include <silent.h>

class AccountBackend;

namespace catalog {
class BalanceHeader;
class HistoryTable;
} // namespace catalog

namespace account::view {

class History : public qontrol::Screen {
    Q_OBJECT

public:
    explicit History(AccountBackend *backend);

public slots:
    void onCoinsUpdated(const CoinState &state);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;

private:
    AccountBackend *m_backend = nullptr;
    CoinState m_state{};
    QWidget *m_main_widget = nullptr;
    catalog::BalanceHeader *m_balance = nullptr;
    catalog::HistoryTable *m_table = nullptr;
};

} // namespace account::view
