#pragma once

#include <QString>
#include <QWidget>
#include <cstdint>
#include <silent.h>

class MusigAccountController;
enum class Endpoint : std::uint8_t;

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
    void onEndpointStateChanged(Endpoint endpoint);
    void onElectrumToggled(bool checked);
    void onReloadUrl();
    void onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                   const EndpointResult &result);

protected:
    void init();
    void doConnect();
    void view();

private:
    MusigAccountController *m_controller = nullptr;
    catalog::Separator *m_separator = nullptr;
    catalog::StatusBarItem *m_electrum_item = nullptr;
    QString m_electrum_url;
    uint64_t m_endpoint_req = 0;
};
