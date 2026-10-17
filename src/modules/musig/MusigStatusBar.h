#pragma once

#include <QString>
#include <QWidget>

class MusigAccountController;

namespace catalog {
class Separator;
class StatusBarItem;
} // namespace catalog

// The Electrum half of sp's StatusBar, a musig account has no scanner.
class MusigStatusBar : public QWidget {
    Q_OBJECT

public:
    explicit MusigStatusBar(MusigAccountController *controller, QWidget *parent = nullptr);

public slots:
    void onElectrumConnected(const QString &address);
    void onElectrumDisconnected();
    void onElectrumToggled(bool checked);
    void onReloadUrl();

protected:
    void init();
    void doConnect();
    void view();

private:
    MusigAccountController *m_controller = nullptr;
    catalog::Separator *m_separator = nullptr;
    catalog::StatusBarItem *m_electrum_item = nullptr;
    QString m_electrum_url;
};
