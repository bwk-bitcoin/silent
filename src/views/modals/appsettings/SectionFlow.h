#pragma once

#include <QPointer>
#include <QString>

#include "interfaces/host.h"
#include "views/modals/AppSettings.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
class ScrollArea;
} // namespace catalog

namespace qontrol {
class Column;
}

namespace modal {

// A module's settings section, under the line saying whose it is. The module owns
// the section widget: it is only lent to the flow while it is open, and handed back
// when the flow goes back or is deleted.
class SectionFlow final : public AppSettingsFlow {
    Q_OBJECT

public:
    SectionFlow(Host *host, Host::SectionId section_id, QString title, const QString &text,
                QWidget *content);
    ~SectionFlow() override;
    // Shows the section.
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onHostSettingsSectionsChanged();

protected:
    void init(const QString &text);
    void doConnect();
    void view();

    // Section view
    void initSection(const QString &text);
    void connectSection();
    [[nodiscard]] auto viewSection() -> catalog::ScrollArea *;
    void showSection();
    void closeSection();

private:
    Host *m_host = nullptr;
    QString m_title;

    catalog::ScrollArea *m_section = nullptr;
    catalog::Label *m_section_text = nullptr;
    qontrol::Column *m_section_slot = nullptr;
    catalog::ModalActions *m_section_actions = nullptr;
    catalog::Button *m_done_btn = nullptr;
    // The module owns its section widget: it is only lent to the slot while open.
    QPointer<QWidget> m_open_section;
    Host::SectionId m_open_section_id = 0;
};

} // namespace modal
