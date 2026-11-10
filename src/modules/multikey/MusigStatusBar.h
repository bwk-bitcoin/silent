#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <silent.h>

class MusigAccountController;
enum class Endpoint : std::uint8_t;

namespace backend {
class IBlockchain;
} // namespace backend

namespace catalog {
class StatusBarItem;
} // namespace catalog

// The status details of a Multikey account in the shared status bar: the Electrum half
// of sp's StatusBar, a musig account has no scanner. With a blockchain backend plugin
// chosen, the connection that plugin reports instead.
class MusigStatusBar : public QWidget {
    Q_OBJECT

public:
    explicit MusigStatusBar(MusigAccountController *controller, QWidget *parent = nullptr);

public slots:
    void onEndpointStateChanged(Endpoint endpoint);
    void onElectrumToggled(bool checked);
    void onReloadUrl();
    void onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                   const EndpointResult &result);
    // The services were assembled again: the item follows the backend now chosen.
    void onBackendChanged();
    void onBackendStateChanged();

protected:
    void init();
    void doConnect();
    void view();

private:
    MusigAccountController *m_controller = nullptr;
    catalog::StatusBarItem *m_electrum_item = nullptr;
    QString m_electrum_url;
    uint64_t m_endpoint_req = 0;
    QPointer<backend::IBlockchain> m_backend;
};
