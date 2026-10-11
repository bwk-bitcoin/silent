#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/signer.h"

#include <QPointer>
#include <QString>
#include <QStringList>
#include <optional>

namespace catalog {
class Button;
class ComboBox;
class FieldGroup;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
} // namespace catalog

namespace qontrol {
class Row;
}

namespace keys {
class KeysModule;
}

namespace keys::modal {

// Fetches the xpub at a chosen path from the key's connected device and caches it,
// opened over the key detail it returns to: picking the path, then the device wait.
// The path is a standard purpose with its account index (and its script type for
// BIP48), or a custom path; the default account is the first one not cached yet.
class FetchXpub final : public catalog::Modal {
    Q_OBJECT

public:
    FetchXpub(KeysModule *module, const QString &fingerprint);

public slots:
    void onNextClicked();
    void onPurposeChanged();
    void onAccountEdited();
    void onInputChanged();
    void onDeviceXpub(ReqId req_id, const QString &xpub);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message);
    void onInstanceRemoved(const QString &id);

protected:
    // The view on display: the primary action acts on it.
    enum class View { Fetch, Fetching };

    void init();
    void doConnect();
    void view();

    // Fetch view
    void initFetch();
    void connectFetch();
    [[nodiscard]] auto viewFetch() -> QWidget *;
    void showFetch();
    [[nodiscard]] auto purpose() const -> QString;
    [[nodiscard]] auto pathFor(const QString &purpose, int account) const -> QString;
    [[nodiscard]] auto path() const -> QString;
    [[nodiscard]] auto freeAccount(const QString &purpose) const -> int;
    [[nodiscard]] auto reason() const -> QString;
    void refresh();
    void showError(const QString &error);
    void fetch();

    // Fetching view
    void initFetching();
    void showFetching();
    void hideFetching();
    void stopFetch();

private:
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    View m_view = View::Fetch;
    // Paths already cached for the key, written m/...
    QStringList m_cached;
    // The account index was typed, so a purpose change keeps it.
    bool m_typed = false;

    QWidget *m_fetch_step = nullptr;
    catalog::ComboBox *m_purpose = nullptr;
    catalog::Label *m_prefix = nullptr;
    catalog::Input *m_account = nullptr;
    catalog::Label *m_suffix = nullptr;
    catalog::ComboBox *m_script = nullptr;
    qontrol::Row *m_template = nullptr;
    catalog::Input *m_custom = nullptr;
    catalog::FieldGroup *m_fields = nullptr;
    catalog::Notice *m_error = nullptr;
    catalog::Button *m_fetch = nullptr;
    catalog::ModalActions *m_actions = nullptr;

    catalog::ModalStatus *m_fetching_step = nullptr;

    QPointer<ISigner> m_signer;
    QString m_instance;
    QString m_path;
    ReqId m_req = 0;
};

} // namespace keys::modal
