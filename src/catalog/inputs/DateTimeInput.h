#pragma once

#include "theme/Palette.h"

#include <QDateTimeEdit>

namespace catalog {

// A date and time with a calendar popup, styled as an input, in the mono font.
class DateTimeInput : public QDateTimeEdit {
    Q_OBJECT

public:
    explicit DateTimeInput(QWidget *parent = nullptr);
    void setWidth(Size s);
    static auto qss(const Palette &p) -> QString;
};

} // namespace catalog
