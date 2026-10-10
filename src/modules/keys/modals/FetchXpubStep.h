#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

namespace catalog {
class Button;
class ComboBox;
class FieldGroup;
class Input;
class Label;
class ModalActions;
class Notice;
} // namespace catalog

namespace qontrol {
class Row;
}

namespace keys::modal {

// Picks the path of an xpub to fetch from a device: a standard purpose with its
// account index (and its script type for BIP48), or a custom path. The default
// account is the first one whose path is not cached yet.
class FetchXpubStep : public QWidget {
    Q_OBJECT

public:
    explicit FetchXpubStep(QWidget *parent = nullptr);
    // Back to BIP84 at the first free account, with no error. `cached` are the
    // paths already cached, written m/...
    void reset(const QStringList &cached);
    void showError(const QString &error);
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void fetchClicked(QString path);

public slots:
    void onPurposeChanged();
    void onAccountEdited();
    void onInputChanged();
    void onFetchClicked();

protected:
    void init();
    void doConnect();
    void view();
    [[nodiscard]] auto purpose() const -> QString;
    [[nodiscard]] auto path() const -> QString;
    [[nodiscard]] auto pathFor(const QString &purpose, int account) const -> QString;
    [[nodiscard]] auto freeAccount(const QString &purpose) const -> int;
    [[nodiscard]] auto reason() const -> QString;
    void refresh();

private:
    QStringList m_cached;
    // The account index was typed, so a purpose change keeps it.
    bool m_typed = false;
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
};

} // namespace keys::modal
