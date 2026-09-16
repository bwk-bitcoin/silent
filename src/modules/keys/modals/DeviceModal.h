#pragma once

#include <optional>

#include <QMap>
#include <QPointer>
#include <QString>

#include <Qontrol>
#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class Button;
class Input;
class Label;
} // namespace catalog

class ISigner;

namespace keys::modal {

// One device, as the Keys tab shows it: what the service already knows about it, plus
// an on demand info request and an xpub request at a path the user can change.
class DeviceModal final : public qontrol::Modal {
    Q_OBJECT

public:
    explicit DeviceModal(ISigner *signer, Network network, QWidget *parent = nullptr);
    ~DeviceModal() override = default;

public slots:
    void onInfoClicked();
    void onXpubClicked();
    void onSignerInfo(ReqId req_id, const QMap<QString, QString> &info);
    void onSignerXpub(ReqId req_id, const QString &xpub);
    void onSignerError(std::optional<ReqId> req_id, const QString &message);

protected:
    void init();
    void doConnect();
    void view();

private:
    // The device can be unplugged while the modal is open, so never a raw pointer.
    QPointer<ISigner> m_signer;
    Network m_network = Network::Bitcoin;
    catalog::Input *m_path_input = nullptr;
    catalog::Label *m_result_label = nullptr;
    catalog::Button *m_info_btn = nullptr;
    catalog::Button *m_xpub_btn = nullptr;
    catalog::Button *m_close_btn = nullptr;
    ReqId m_pending_info = 0;
    ReqId m_pending_xpub = 0;
};

} // namespace keys::modal
