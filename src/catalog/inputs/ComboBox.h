#pragma once

#include "theme/Palette.h"
#include <QComboBox>

namespace catalog {

enum class ComboBoxRole { Default };

class ComboBox : public QComboBox {
    Q_OBJECT

public:
    explicit ComboBox(ComboBoxRole role = ComboBoxRole::Default, QWidget *parent = nullptr);
    void setRole(ComboBoxRole role);
    [[nodiscard]] auto role() const -> ComboBoxRole;
    void setWidth(Size s);
    // A disabled entry is greyed out in the popup and cannot be picked, by click or key.
    void setItemEnabled(int index, bool enabled);
    void showPopup() override;
    static auto qss(const Palette &p) -> QString;
    static auto renderChevron(const QColor &color) -> QString;

private:
    ComboBoxRole m_role = ComboBoxRole::Default;
    void applyRole();
};

} // namespace catalog
