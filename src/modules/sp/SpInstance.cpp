#include "SpInstance.h"

#include "AccountController.h"
#include "AccountWidget.h"
#include "SpAccount.h"

#include "interfaces/feed.h"
#include "interfaces/signer.h"
#include "interfaces/theme.h"

#include <QDebug>
#include <common.h>

namespace {
class InertFeed final : public IFeed {
public:
    using IFeed::IFeed;

    [[nodiscard]] bool implemented() const override { return false; }
    ReqId raw(const QByteArray &request) override {
        Q_UNUSED(request);
        return 0;
    }
};

class InertSigner final : public ISigner {
public:
    using ISigner::ISigner;

    [[nodiscard]] bool implemented() const override { return false; }
    [[nodiscard]] QString id() const override { return QString(); }
    [[nodiscard]] QString fingerprint() const override { return QString(); }
    [[nodiscard]] QString walletName() const override { return QString(); }
    [[nodiscard]] plugin::SignerState state() const override {
        return plugin::SignerState::Unsupported;
    }
    [[nodiscard]] QString stateDetail() const override { return QString(); }
    ReqId init() override { return 0; }
    ReqId info() override { return 0; }
    ReqId getXpub(const QString &path) override {
        Q_UNUSED(path);
        return 0;
    }
    ReqId isDescriptorRegistered(const QString &descriptor) override {
        Q_UNUSED(descriptor);
        return 0;
    }
    ReqId registerDescriptor(const QString &descriptor) override {
        Q_UNUSED(descriptor);
        return 0;
    }
    ReqId signWithDescriptor(const QString &psbt_base64, const QString &descriptor) override {
        Q_UNUSED(psbt_base64);
        Q_UNUSED(descriptor);
        return 0;
    }
    ReqId raw(const QByteArray &request) override {
        Q_UNUSED(request);
        return 0;
    }
};

class InertThemeProvider final : public IThemeProvider {
public:
    using IThemeProvider::IThemeProvider;

    [[nodiscard]] bool implemented() const override { return false; }
    ReqId themes() override { return 0; }
    ReqId palette(const QString &name) override {
        Q_UNUSED(name);
        return 0;
    }
    ReqId raw(const QByteArray &request) override {
        Q_UNUSED(request);
        return 0;
    }
};
} // namespace

SpInstance::SpInstance(QString id) : m_id(std::move(id)) {
    m_account_widget = new AccountWidget(m_id);
    m_account_controller = m_account_widget->controller();

    m_account = std::make_unique<SpAccount>(m_account_controller);
    m_feed = std::make_unique<InertFeed>();
    m_signer = std::make_unique<InertSigner>();
    m_theme = std::make_unique<InertThemeProvider>();

    m_tab_id = Host::get()->openTab(m_account_widget, this);
    if (m_tab_id == 0) {
        qCritical() << "SpInstance: failed to open tab for account" << m_id;
        m_account_widget->deleteLater();
        m_account_widget = nullptr;
    } else {
        Host::get()->setTabTitle(m_tab_id, m_id);
    }
}

QString SpInstance::id() const {
    return m_id;
}

void SpInstance::stop() {
    if (m_stopped) {
        return;
    }
    m_stopped = true;

    // Detach the tab from view immediately, but keep the widget alive: the
    // controller tears the account down in the background and emits stopped()
    // once its notification thread is gone, at which point the widget (and the
    // controller it owns) can be deleted safely.
    if (m_tab_id != 0) {
        Host::get()->closeTab(m_tab_id, false);
        m_tab_id = 0;
    }

    if (m_account_controller != nullptr && m_account_widget != nullptr) {
        QObject::connect(m_account_controller, &AccountController::stopped, m_account_widget,
                         &QWidget::deleteLater, qontrol::UNIQUE);
        m_account_controller->stop();
    } else if (m_account_widget != nullptr) {
        m_account_widget->deleteLater();
    }

    // The widget now owns its own teardown; drop our non-owning references.
    m_account_widget = nullptr;
    m_account_controller = nullptr;
}

IAccount *SpInstance::account() {
    return m_account.get();
}

IFeed *SpInstance::feed() {
    return m_feed.get();
}

ISigner *SpInstance::signer() {
    return m_signer.get();
}

IThemeProvider *SpInstance::theme() {
    return m_theme.get();
}
