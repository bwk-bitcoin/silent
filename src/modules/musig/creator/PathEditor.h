#pragma once

#include "interfaces/keys.h"
#include "modules/musig/creator/SpendPath.h"

#include <QList>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <optional>

class QIntValidator;

namespace catalog {
class Checkbox;
class ComboBox;
class DateTimeInput;
class FieldGroup;
class Input;
class Label;
} // namespace catalog

namespace musig::modal {

// One spend path being edited: its optional name, the keys it picks among the
// network's and those that sign every spend (at least one stays optional), the
// signatures it needs, and when it opens.
class PathEditor : public QWidget {
    Q_OBJECT

public:
    explicit PathEditor(const QList<KeyEntry> &keys, QWidget *parent = nullptr);
    void setPath(const SpendPath &path);
    [[nodiscard]] auto path() const -> SpendPath;
    // The chain tip a renewable count is shown from, none while it is not known.
    void setTip(std::optional<std::uint32_t> tip);

signals:
    void edited();

public slots:
    void onNameEdited(const QString &text);
    void onKeyToggled(bool checked);
    void onMandatoryToggled(bool checked);
    void onThresholdChanged(int index);
    void onLockKindChanged(int index);
    void onCountEdited(const QString &text);
    void onUnitChanged(int index);
    void onDateChanged(const QDateTime &date);

protected:
    void init();
    void doConnect();
    void view();
    // The path from the widgets, the threshold kept within its range.
    void readPath();
    // Shows the mandatory boxes, the threshold items and the timelock inputs `m_path`
    // has, without emitting edited().
    void refresh();
    void refreshThresholds();

private:
    QList<KeyEntry> m_keys;
    SpendPath m_path;
    std::optional<std::uint32_t> m_tip;
    catalog::Input *m_name_input = nullptr;
    // One entry per m_keys key, same order.
    QList<catalog::Checkbox *> m_key_checks;
    QList<catalog::Checkbox *> m_mandatory_checks;
    QWidget *m_keys_table = nullptr;
    catalog::FieldGroup *m_fields = nullptr;
    catalog::ComboBox *m_threshold_combo = nullptr;
    catalog::ComboBox *m_lock_combo = nullptr;
    catalog::Input *m_count_input = nullptr;
    QIntValidator *m_count_validator = nullptr;
    catalog::ComboBox *m_unit_combo = nullptr;
    catalog::DateTimeInput *m_date_input = nullptr;
    catalog::Label *m_hint_label = nullptr;
};

} // namespace musig::modal
