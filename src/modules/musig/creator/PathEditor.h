#pragma once

#include "interfaces/keys.h"

#include <QList>
#include <QWidget>
#include <silent.h>

class QComboBox;
class QSpinBox;

namespace catalog {
class Button;
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
    [[nodiscard]] auto path(Network network) const -> PolicyPath;

signals:
    void edited();
    void removeRequested(PathEditor *path);

public slots:
    void onSemanticChanged(int index);
    void onThresholdChanged(int value);
    void onLocktimeKindChanged(int index);
    void onLocktimeValueChanged(int value);
    void onKeyToggled(bool checked);
    void onKeyTypeChanged(int index);
    void onAccountChanged(int value);
    void onMandatoryToggled(bool checked);
    void onRemoveClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    QList<KeyEntry> m_keys;
    QComboBox *m_semantic_combo = nullptr;
    QSpinBox *m_threshold_spin = nullptr;
    QComboBox *m_locktime_combo = nullptr;
    QSpinBox *m_locktime_spin = nullptr;
    qontrol::Column *m_keys_column = nullptr;
    catalog::Button *m_remove_btn = nullptr;
};

} // namespace musig::modal
