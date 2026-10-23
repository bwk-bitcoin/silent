#pragma once

#include "views/modals/AppSettings.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
} // namespace catalog

namespace modal {

class RemoveFlow final : public AppSettingsFlow {
    Q_OBJECT

public:
    explicit RemoveFlow(QString name);
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    QString m_name;
    QWidget *m_body = nullptr;
    catalog::Label *m_question = nullptr;
    catalog::Notice *m_warning = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_remove_btn = nullptr;
};

} // namespace modal
