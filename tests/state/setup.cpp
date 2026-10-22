#include "FakeUnlockDevices.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QSignalSpy>
#include <QThread>
#include <Qontrol>
#include <functional>
#include <memory>
#include <silent.h>

#define private public
#define protected public
#include "state/StateSetup.h"
#include "state/UnlockDevices.h"
#include "state/statesetup/DeviceFlow.h"
#include "state/statesetup/PasswordFlow.h"
#undef protected
#undef private

#include "MainWindow.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/feedback/Notice.h"
#include "catalog/inputs/Input.h"
#include "i18n/I18nManager.h"
#include "resources/font/noto_sans.h"
#include "resources/font/noto_sans_bold.h"
#include "resources/font/noto_sans_mono.h"
#include "resources/font/noto_sans_mono_bold.h"
#include "theme/Theme.h"

namespace {

constexpr auto PASSWORD = "setup-password-67";
constexpr auto XPUB = "tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXT"
                      "exf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa";

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("state setup: %s", message);
    }
}

void waitFor(const std::function<bool()> &ready) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 20000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "operation completed within 20 seconds");
}

void disposeEvents() {
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

void fillPassword(state::PasswordFlow &flow, const QString &password = PASSWORD) {
    flow.m_password_input->setText(password);
    flow.m_confirm_input->setText(password);
    flow.onPasswordEdited();
}

void refuseDeparture(state::StateSetup &setup, state::StateSetupFlow &flow) {
    require(setup.m_busy, "router is busy");
    setup.onFlowBack();
    setup.reject();
    require(!setup.close(), "window close refused while irreversible work runs");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&setup, &escape);
    setup.done(QDialog::Rejected);
    setup.onPasswordChosen();
    require(setup.isVisible() && setup.m_flow == &flow, "Back/reject/close/Escape/done keep flow");
}

void lifecycle() {
    state::StateSetup setup(Network::Regtest);
    setup.show();
    require(setup.findChildren<state::StateSetupFlow *>().isEmpty(), "Choice has no flows");
    setup.onPasswordChosen();
    auto *password = qobject_cast<state::PasswordFlow *>(setup.m_flow);
    QPointer<state::PasswordFlow> oldPassword(password);
    fillPassword(*password);
    password->onBackClicked();
    require(password->m_password_input->text().isEmpty() &&
                password->m_confirm_input->text().isEmpty(),
            "Back clears both secret fields before deferred deletion");
    disposeEvents();
    require(oldPassword.isNull(), "Password flow destroyed on Back");
    setup.onPasswordChosen();
    password = qobject_cast<state::PasswordFlow *>(setup.m_flow);
    require(password->m_password_input->text().isEmpty() &&
                password->m_confirm_input->text().isEmpty(),
            "Password revisit has two fresh fields");
    require(!password->m_create->isEnabled(), "fresh Password cannot create");
    fillPassword(*password, "short");
    require(!password->m_create->isEnabled(), "short password refused");
    password->onNextClicked();
    require(!setup.m_busy && !::state_unlock_info().exists, "short password creates no key");
    fillPassword(*password);
    password->m_confirm_input->setText("not-matching");
    password->onPasswordEdited();
    password->onNextClicked();
    require(!setup.m_busy && !::state_unlock_info().exists, "mismatched password creates no key");
    password->onBackClicked();
    disposeEvents();

    setup.onDeviceChosen();
    auto *device = qobject_cast<state::DeviceFlow *>(setup.m_flow);
    QPointer<state::DeviceFlow> oldDevice(device);
    QPointer<state::UnlockDevices> oldDiscovery(device->m_devices);
    require(setup_test::discoveries == 1 && setup_test::pending == 1,
            "one pending discovery on entry");
    HwiEvent event;
    event.flag = HwiEventFlag::DevicesChanged;
    emit device->m_devices->hwiEventReceived(event);
    device->onBackClicked();
    require(oldDiscovery.isNull() && setup_test::releases == 1,
            "Back immediately releases discovery");
    disposeEvents();
    require(oldDevice.isNull(), "Device flow destroyed on Back");
    waitFor([] { return setup_test::pending == 0; });
    QApplication::processEvents();
    require(setup_test::deliveries == 0 && setup.m_flow == nullptr,
            "queued and pending discovery results do not reach disposed flow");
    setup.onDeviceChosen();
    device = qobject_cast<state::DeviceFlow *>(setup.m_flow);
    require(setup_test::discoveries == 2 && device->m_selected_id.isEmpty(),
            "Device revisit starts fresh discovery");
    device->onBackClicked();
    disposeEvents();
    require(setup_test::releases == 2, "both discoveries released");
    waitFor([] { return setup_test::pending == 0; });
    setup.onPasswordChosen();
    password = qobject_cast<state::PasswordFlow *>(setup.m_flow);
    fillPassword(*password);
    setup.reject();
    disposeEvents();
    require(setup.m_flow == nullptr && !setup.isVisible(), "idle close disposes flow");
    qInfo("lifecycle: fresh Password, discovery starts=2 releases=2 stale deliveries=0");
}

void passwordSuccess(bool destroy) {
    auto *setup = new state::StateSetup(Network::Regtest);
    setup->show();
    setup->onPasswordChosen();
    auto *flow = qobject_cast<state::PasswordFlow *>(setup->m_flow);
    fillPassword(*flow);
    QSignalSpy finished(setup, &QDialog::accepted);
    flow->onNextClicked();
    require(flow->m_password_input->text().isEmpty() && flow->m_confirm_input->text().isEmpty(),
            "password fields cleared before worker starts");
    refuseDeparture(*setup, *flow);
    flow->onBackClicked();
    flow->onNextClicked();
    require(setup->m_busy && finished.isEmpty(), "busy flow refuses Back and duplicate creation");
    if (destroy) {
        QPointer<state::PasswordFlow> oldFlow(flow);
        delete setup;
        require(oldFlow.isNull(), "forced destruction deletes worker receiver");
        waitFor([] { return ::state_unlock_info().exists; });
        // Wait for the worker to finish, not just the durable file becoming visible.
        waitFor([] { return ::state_unlock_with_password(PASSWORD).empty(); });
        disposeEvents();
        qInfo("password destruction: late worker delivery safe, actual saved key unlocks");
        return;
    }
    waitFor([&finished] { return finished.count() == 1; });
    require(!setup->m_busy && setup->m_flow == nullptr && setup->result() == QDialog::Accepted,
            "password success unlocks modal and accepts once");
    auto info = ::state_unlock_info();
    require(info.exists && !info.device, "password key persisted");
    ::state_lock();
    require(!::state_unlock_with_password("incorrect-password").empty(), "wrong password refused");
    require(::state_unlock_with_password(PASSWORD).empty(),
            "saved password unlocks actual Rust state");
    disposeEvents();
    delete setup;
    qInfo(
        "password success: busy departure refused, persisted key unlocks, wrong password refused");
}

void passwordFailure() {
    QDir directory(QDir::homePath() + "/.silent");
    require(directory.mkdir("unlock.key"), "filesystem refusal fixture created");
    state::StateSetup setup(Network::Regtest);
    setup.show();
    setup.onPasswordChosen();
    auto *flow = qobject_cast<state::PasswordFlow *>(setup.m_flow);
    fillPassword(*flow);
    flow->onNextClicked();
    waitFor([&setup] { return !setup.m_busy; });
    require(setup.isVisible() && flow->m_view == state::PasswordFlow::View::Password &&
                flow->m_password_error->isVisible(),
            "write failure restores editable Password");
    require(flow->m_password_input->text().isEmpty() && flow->m_confirm_input->text().isEmpty(),
            "failure does not restore secrets");
    require(!::state_unlock_info().exists, "write failure creates no key");
    require(directory.rename("unlock.key", "unlock-key-blocker"), "filesystem fixture released");
    fillPassword(*flow);
    QSignalSpy finished(&setup, &QDialog::accepted);
    flow->onNextClicked();
    waitFor([&finished] { return finished.count() == 1; });
    require(!setup.m_busy && setup.result() == QDialog::Accepted,
            "retry succeeds after write failure");
    disposeEvents();
    qInfo("password failure: write error shown, fresh fields, retry succeeds");
}

void passwordExisting() {
    require(::state_create_with_password(PASSWORD).empty(), "original password state created");
    QFile key(QDir::homePath() + "/.silent/unlock.key");
    QFile check(QDir::homePath() + "/.silent/unlock.bip138");
    require(key.open(QIODevice::ReadOnly) && check.open(QIODevice::ReadOnly),
            "original unlock files readable");
    auto originalKey = key.readAll();
    auto originalCheck = check.readAll();
    key.close();
    check.close();
    state::StateSetup setup(Network::Regtest);
    setup.show();
    setup.onPasswordChosen();
    auto *flow = qobject_cast<state::PasswordFlow *>(setup.m_flow);
    fillPassword(*flow, "second-instance-password");
    flow->onNextClicked();
    waitFor([&setup] { return !setup.m_busy; });
    require(setup.isVisible() && flow->m_password_error->isVisible(),
            "stale setup presents Rust refusal without accepting");
    require(key.open(QIODevice::ReadOnly) && check.open(QIODevice::ReadOnly),
            "unlock files remain readable");
    require(key.readAll() == originalKey && check.readAll() == originalCheck,
            "stale password setup preserves both original files byte for byte");
    flow->onBackClicked();
    disposeEvents();
    ::state_lock();
    require(::state_unlock_with_password(PASSWORD).empty(), "original password still unlocks");
    qInfo("stale password setup: UI error, original bytes preserved, original password unlocks");
}

void deviceSuccess(bool destroy) {
    setup_test::listDevice = true;
    auto *setup = new state::StateSetup(Network::Regtest);
    setup->show();
    setup->onDeviceChosen();
    auto *flow = qobject_cast<state::DeviceFlow *>(setup->m_flow);
    auto rows = flow->m_device_rows->findChildren<catalog::SelectRow *>();
    require(rows.size() == 1, "one software device listed");
    emit rows.first()->clicked();
    require(flow->m_device_continue->isEnabled(), "selection enables Continue");
    setup_test::refuseRequest = true;
    flow->onNextClicked();
    require(!setup->m_busy && flow->m_device_error->isVisible(),
            "unreachable request reports error");
    setup_test::refuseRequest = false;
    flow->onNextClicked();
    refuseDeparture(*setup, *flow);
    auto reqId = flow->m_xpub_req;
    flow->onXpubFetched(reqId + 1, XPUB);
    flow->onXpubFailed(reqId + 1, "stale");
    require(flow->m_xpub_req == reqId && setup->m_busy, "stale responses do not finish request");
    flow->onXpubFailed(reqId, "test refusal");
    require(!setup->m_busy && flow->m_view == state::DeviceFlow::View::Device &&
                flow->m_device_error->isVisible(),
            "device refusal restores Device and departure");
    flow->onNextClicked();
    reqId = flow->m_xpub_req;
    flow->onXpubFetched(reqId, "invalid-xpub");
    waitFor([setup] { return !setup->m_busy; });
    require(flow->m_device_error->isVisible() && !::state_unlock_info().exists,
            "invalid software xpub fails without creating key");
    flow->onNextClicked();
    reqId = flow->m_xpub_req;
    auto path = ::state_device_path(flow->m_index);
    QSignalSpy finished(setup, &QDialog::accepted);
    HwiEvent event;
    event.flag = HwiEventFlag::Xpub;
    event.req_id = reqId;
    event.payload = XPUB;
    emit flow->m_devices->hwiEventReceived(event);
    waitFor([flow] { return flow->m_result != nullptr; });
    refuseDeparture(*setup, *flow);
    flow->onBackClicked();
    flow->onXpubFetched(reqId, XPUB);
    require(setup->m_busy, "commit refuses departure and duplicate xpub");
    if (destroy) {
        QPointer<state::DeviceFlow> oldFlow(flow);
        delete setup;
        require(oldFlow.isNull(), "forced destruction deletes device worker receiver");
        waitFor([] { return ::state_unlock_info().exists; });
        waitFor([] { return ::state_unlock_with_device(XPUB).empty(); });
        waitFor([] { return setup_test::pending == 0; });
        disposeEvents();
        qInfo("device destruction: late worker delivery safe, actual saved key unlocks");
        return;
    }
    waitFor([&finished] { return finished.count() == 1; });
    auto info = ::state_unlock_info();
    require(info.exists && info.device && info.path == path,
            "random device path persisted exactly");
    require(!setup->m_busy && setup->m_flow == nullptr && setup->result() == QDialog::Accepted,
            "device success accepts once");
    ::state_lock();
    require(!::state_unlock_with_device("invalid-xpub").empty(), "invalid unlock key refused");
    require(::state_unlock_with_device(XPUB).empty(),
            "software xpub unlocks actual saved Rust key");
    disposeEvents();
    require(setup_test::releases == 1, "successful pairing releases discovery");
    waitFor([] { return setup_test::pending == 0; });
    delete setup;
    qInfo("device success: failures/retry, busy refusal, random path persisted, saved key unlocks");
}

void screenshots(const QString &directory) {
    MainWindow window;
    window.resize(1200, 800);
    window.show();
    for (int i = 0; i < 30; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    state::StateSetup setup(Network::Regtest);
    setup.show();
    auto shot = [&](const QString &name) {
        if (QApplication::focusWidget() != nullptr) {
            QApplication::focusWidget()->clearFocus();
        }
        for (int i = 0; i < 30; ++i) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        auto image = window.grab();
        QPainter painter(&image);
        painter.drawPixmap(setup.mapToGlobal(QPoint()) - window.mapToGlobal(QPoint()),
                           setup.grab());
        painter.end();
        require(image.save(directory + "/" + name + ".png"), "full-window screenshot saved");
    };
    shot("choice");
    setup.onDeviceChosen();
    shot("device");
    qobject_cast<state::DeviceFlow *>(setup.m_flow)->onBackClicked();
    disposeEvents();
    setup.onPasswordChosen();
    shot("password");
    auto *password = qobject_cast<state::PasswordFlow *>(setup.m_flow);
    fillPassword(*password);
    password->onBackClicked();
    disposeEvents();
    setup.onPasswordChosen();
    shot("password-fresh");
    setup.reject();
    disposeEvents();
}

} // namespace

auto main(int argc, char **argv) -> int {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QFontDatabase::addApplicationFontFromData(QByteArray(
        reinterpret_cast<const char *>(embedded_font::NOTO_SANS), embedded_font::NOTO_SANS_SIZE));
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_MONO),
                   embedded_font::NOTO_SANS_MONO_SIZE));
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_BOLD),
                   embedded_font::NOTO_SANS_BOLD_SIZE));
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_MONO_BOLD),
                   embedded_font::NOTO_SANS_MONO_BOLD_SIZE));
    Theme::init();
    Theme::get()->apply();
    i18n::I18nManager::get()->init(&app);
    require(argc >= 2, "test mode provided");
    QString mode = QString::fromLocal8Bit(argv[1]);
    if (mode == "lifecycle") {
        lifecycle();
    } else if (mode == "password" || mode == "password-destroy") {
        passwordSuccess(mode == "password-destroy");
    } else if (mode == "password-failure") {
        passwordFailure();
    } else if (mode == "password-existing") {
        passwordExisting();
    } else if (mode == "device" || mode == "device-destroy") {
        deviceSuccess(mode == "device-destroy");
    } else if (mode == "screenshots" && argc == 3) {
        screenshots(QString::fromLocal8Bit(argv[2]));
    } else {
        qFatal("unknown test mode");
    }
}
