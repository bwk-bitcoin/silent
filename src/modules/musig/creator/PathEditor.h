#pragma once

#include "interfaces/keys.h"

#include <QList>
#include <QString>
#include <QWidget>
#include <silent.h>

class QIntValidator;

namespace catalog {
class Button;
class Checkbox;
class ComboBox;
class Input;
class Label;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig::modal {

// One path.
class PathEditor : public QWidget {
    Q_OBJECT

public:
    PathEditor(const QList<KeyEntry> &keys, QWidget *parent = nullptr);
    // Every key at BIP-87 account 0 of `network`, account_xpub empty.
    [[nodiscard]] auto path(Network network) const -> PolicyPath;
    void setTitle(const QString &title);
    void setRemovable(bool removable);

signals:
    void edited();
    void removeRequested(PathEditor *path);

public slots:
    void onSemanticChanged(int index);
    void onThresholdChanged(int index);
    void onLocktimeKindChanged(int index);
    void onLocktimeValueChanged(const QString &text);
    void onKeyToggled(bool checked);
    void onKeyTypeChanged(int index);
    void onMandatoryToggled(bool checked);
    void onRemoveClicked();

protected:
    void init();
    void doConnect();
    void view();
    // Threshold items 1 to the checked key count, keeping the current one in range.
    void refreshThresholds();
    // Shows the threshold and the mandatory flags the semantic has.
    void applySemantic();
    [[nodiscard]] auto semantic() const -> PolicySemantic;
    [[nodiscard]] auto locktime() const -> PolicyLocktime;

private:
    QList<KeyEntry> m_keys;
    catalog::Label *m_title_label = nullptr;
    catalog::ComboBox *m_semantic_combo = nullptr;
    catalog::ComboBox *m_threshold_combo = nullptr;
    catalog::ComboBox *m_locktime_combo = nullptr;
    catalog::Input *m_locktime_input = nullptr;
    QIntValidator *m_locktime_validator = nullptr;
    qontrol::Column *m_keys_column = nullptr;
    // One entry per m_keys key, same order.
    QList<catalog::Checkbox *> m_key_checks;
    QList<catalog::ComboBox *> m_key_type_combos;
    QList<catalog::Checkbox *> m_mandatory_checks;
    catalog::Button *m_remove_btn = nullptr;
};

} // namespace musig::modal
