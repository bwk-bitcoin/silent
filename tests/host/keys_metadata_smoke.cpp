#include "AppController.h"
#include "KeysBoundaryProbe.h"
#include "MockPlugin.h"
#include "SpSigningProbe.h"
#include "ViewServices.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldNote.h"
#include "host/CallerHandles.h"
#include "host/Host.h"
#include "modules/keys/KeysModule.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigPlugin.h"
#include "modules/sp/SpPlugin.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <common.h>
#include <cstdint>
#include <limits>
#include <memory>
#include <silent.h>
#include <string>

#define private public
#define protected public
#include "modules/musig/RegisterDescriptor.h"
#include "modules/musig/SignModal.h"
#include "modules/musig/creator/CreatePolicy.h"
#include "modules/sp/modals/CreateAccount.h"
#undef protected
#undef private

namespace smoke {
namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("Keys metadata: %s", message);
    }
}

class MetadataKeys final : public DeferredKeys {
public:
    void setEntries(const QList<KeyEntry> &entries) {
        m_entries = entries;
    }

    [[nodiscard]] auto dispatches() const -> int {
        return requests + m_registrations + m_coordinates;
    }

    void registerDescriptor([[maybe_unused]] ReqId req_id, [[maybe_unused]] plugin::Network network,
                            [[maybe_unused]] const QString &fingerprint,
                            [[maybe_unused]] const QString &descriptor) override {
        ++m_registrations;
    }

    void coordinate([[maybe_unused]] ReqId req_id,
                    [[maybe_unused]] const plugin::SigningRequest &request) override {
        ++m_coordinates;
    }

protected:
    [[nodiscard]] auto readKeys([[maybe_unused]] plugin::Network network) const
        -> KeyListResult override {
        return {.is_ok = true, .entries = m_entries};
    }

private:
    QList<KeyEntry> m_entries;
    int m_registrations = 0;
    int m_coordinates = 0;
};

auto hasMetadataError(QWidget *widget) -> bool {
    return std::ranges::any_of(widget->findChildren<QLabel *>(), [](QLabel *label) {
        return label->text() == QStringLiteral("invalid key route metadata");
    });
}

void malformedViews(MusigAccountController &controller) {
    modal::CreateAccount create;
    create.show();
    QApplication::processEvents();
    require(create.m_keys.empty() && create.m_key_rows.empty(),
            "SP creation never renders malformed metadata as an ordinary no-route row");
    require(create.m_fetch_error->isVisible() && hasMetadataError(create.m_fetch_error),
            "SP creation presents the metadata failure");
    require(!create.m_create->isEnabled(), "SP creation stays disabled");
    create.reject();

    musig::modal::CreatePolicy policy;
    policy.m_fingerprints = {QStringLiteral("00112233")};
    policy.refreshRegisterRows();
    policy.show();
    QApplication::processEvents();
    require(policy.m_register_rows->findChildren<catalog::SelectRow *>().empty(),
            "invalid metadata is not displayed as registration Not Needed");
    require(policy.m_status_label->isVisible() &&
                policy.m_status_label->text() == QStringLiteral("invalid key route metadata"),
            "policy creation presents the metadata failure");
    policy.onNextClicked();
    policy.onRegisterRowClicked();
    require(!policy.m_paths_next_btn->isEnabled(),
            "invalid metadata cannot advance policy creation");
    policy.reject();

    musig::modal::RegisterDescriptor registration(&controller);
    registration.show();
    QApplication::processEvents();
    require(registration.m_rows->findChildren<catalog::SelectRow *>().empty() &&
                registration.m_lead->text() == QStringLiteral("invalid key route metadata"),
            "registration presents the failure instead of an ordinary route row");
    registration.onRowClicked();
    registration.reject();

    ViewServices services;
    musig::SignModal signing(&controller, &services.blockchain);
    signing.show();
    QApplication::processEvents();
    require(signing.m_rows->findChildren<catalog::SelectRow *>().empty() &&
                signing.m_notice->isVisible() && hasMetadataError(signing.m_notice),
            "signing presents a failure, not a no-route row");
    require(!signing.m_continue_btn->isEnabled() && !signing.m_broadcast_btn->isEnabled(),
            "signing actions stay disabled");
    signing.onCosignerClicked();
    signing.onBroadcastClicked();
    require(services.blockchain.requests.isEmpty(), "a disabled Broadcast reaches no provider");
    signing.reject();
}
} // namespace

void keysMetadataSmoke(keys::KeysModule &module) {
    AppController::get()->registerBuiltinPlugin(std::make_unique<SpPlugin>());
    AppController::get()->registerBuiltinPlugin(std::make_unique<MusigPlugin>());
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "real host exists");
    MetadataKeys provider;
    IKeys *external = &provider;
    KeyEntry valid{.fingerprint = QStringLiteral("00112233")};
    require(plugin::validateKeyEntry(valid), "Unknown is valid metadata in both fields");
    provider.setEntries({valid});
    require(external->keys(plugin::Network::Regtest).is_ok,
            "a real public IKeys provider publishes valid Unknown");
    host->setKeys(&provider, &module);
    host::KeysHandle handle(host, &module, &provider);
    auto control = handle.keys(plugin::Network::Regtest);
    require(control.is_ok && control.entries.size() == 1 &&
                control.entries.first().route == plugin::KeyRouteKind::Unknown,
            "caller handle preserves valid Unknown");
    ErrorKindSpy errors;
    QObject::connect(&handle, &IKeys::error, &errors, &ErrorKindSpy::onError, qontrol::UNIQUE);
    KeysBoundaryProbe coordination;
    QObject::connect(&handle, &IKeys::coordinationError, &coordination,
                     &KeysBoundaryProbe::onCoordinationError, qontrol::UNIQUE);
    auto accountDescriptor =
        QStringLiteral("tr(musig("
                       "tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtm"
                       "nUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa,"
                       "tpubDDzMtUDKwjvTehAmAfwYiBGzUFDcF8AyEaJf78JhQam5knvsGTgqF7Vvh"
                       "uuDZLYityND66FG88sS3q28Yi5Ysumvx5DfMgdm6nc6zntvpHM"
                       ")/<0;1>/*)");
    auto name = QStringLiteral("smoke-keys-metadata-account");
    auto config = ::new_config(rust::String(name.toStdString()), Network::Testnet,
                               rust::String(accountDescriptor.toStdString()), rust::String(""),
                               rust::String(""), 546, rust::String("musig"));
    require(config->save().empty(), "save isolated Testnet MuSig account");
    MusigAccountController controller(name, nullptr);
    QElapsedTimer opening;
    opening.start();
    while (controller.accountLoading() && opening.elapsed() < 30000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(!controller.accountLoading(), "asynchronous MuSig opening completes");
    require(controller.accountLoadError().isEmpty() && !controller.endpointOperationsBusy() &&
                controller.hasAccountMetadata() && controller.getAccount().has_value() &&
                controller.getAccount().value()->is_ok(),
            "real offline MuSig account opens with its metadata");
    auto canonical = ::validate_musig_descriptor(rust::String(accountDescriptor.toStdString()));
    require(canonical.is_ok && controller.network() == Network::Testnet &&
                std::string(controller.getDescriptor()) == std::string(canonical.descriptor),
            "loaded controller retains the saved Testnet network and Rust-canonical descriptor");
    for (auto invalid : {256U, std::numeric_limits<std::uint32_t>::max()}) {
        for (bool descriptor : {false, true}) {
            auto malformed = valid;
            (descriptor ? malformed.descriptor_route : malformed.route) =
                static_cast<plugin::KeyRouteKind>(invalid);
            require(!plugin::validateKeyEntry(malformed),
                    "pure validation rejects unknown integers");
            provider.setEntries({valid, malformed});
            auto direct = external->keys(plugin::Network::Regtest);
            require(!direct.is_ok && direct.kind == plugin::SignError::InvalidRequest &&
                        direct.entries.empty(),
                    "direct lookup rejects the entire malformed list");
            auto scoped = handle.keys(plugin::Network::Regtest);
            require(!scoped.is_ok && scoped.kind == plugin::SignError::InvalidRequest &&
                        scoped.entries.empty(),
                    "caller-scoped lookup rejects the entire malformed list");
            auto previousErrors = errors.count();
            auto sign = handle.newRequest();
            handle.sign(sign, plugin::Network::Regtest, {}, {}, {}, {});
            require(errors.count() == previousErrors + 1 && errors.reqId() == sign &&
                        errors.kind() == plugin::SignError::InvalidRequest,
                    "signing fails with its correlated typed metadata error");
            auto registration = handle.newRequest();
            handle.registerDescriptor(registration, plugin::Network::Regtest, {}, {});
            require(errors.count() == previousErrors + 2 && errors.reqId() == registration &&
                        errors.kind() == plugin::SignError::InvalidRequest,
                    "registration fails with its correlated typed metadata error");
            auto previousCoordination = coordination.answers;
            plugin::SigningRequest request;
            handle.coordinate(handle.newRequest(), request);
            require(coordination.answers == previousCoordination + 1 &&
                        coordination.kind == plugin::SpendError::InvalidRequest,
                    "coordination refuses malformed metadata before dispatch");
            malformedViews(controller);
            require(provider.dispatches() == 0, "no signing or registration reaches the provider");
        }
    }
    SignalSpy stopped;
    QObject::connect(&controller, &MusigAccountController::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    controller.stop();
    QElapsedTimer stopping;
    stopping.start();
    while (stopped.count() == 0 && stopping.elapsed() < 30000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(stopped.count() == 1, "MuSig account shutdown drains its receiver");
    require(::delete_musig_account(rust::String(name.toStdString())).empty(),
            "remove MuSig test account");
    provider.setEntries({valid});
    require(handle.keys(plugin::Network::Regtest).is_ok, "valid metadata recovers after rejection");
    host->setKeys(module.bus(), &module);
}

} // namespace smoke
