#pragma once

#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QAbstractButton;
class QButtonGroup;
class QWidget;

namespace qontrol {
class Panel;
}

namespace account {

struct MenuEntry {
    QString name;
    QString label;
    QIcon icon;
    bool bottom = false;
};

auto accountMenuEntries() -> QList<MenuEntry>;

// Optional UI composition, not a plugin interface. Panels remain controller-owned.
class AccountShell : public QObject {
    Q_OBJECT

public:
    AccountShell(QList<MenuEntry> entries, QWidget *status, QWidget *parent);
    [[nodiscard]] auto widget() const -> QWidget *;
    void loadPanel(qontrol::Panel *panel);

signals:
    void panelRequested(const QString &name);

public slots:
    void onMenuClicked(int id);

protected:
    [[nodiscard]] auto button(const QString &name) const -> QAbstractButton *;

private:
    QList<MenuEntry> m_entries;
    QWidget *m_widget = nullptr;
    QWidget *m_screen_container = nullptr;
    QButtonGroup *m_buttons = nullptr;
    QPointer<qontrol::Panel> m_current_panel;
};

} // namespace account
