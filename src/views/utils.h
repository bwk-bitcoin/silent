#pragma once

#include "catalog/format.h"
#include "theme/Palette.h"
#include <cstdint>
#include <qwidget.h>

// Layout constants: aliases from design system
const int MARGIN = resolve(Padding::XL);
const int LEFT_MARGIN = MARGIN;
const int TOP_MARGIN = MARGIN;
const int RIGHT_MARGIN = MARGIN;
const int BOTTOM_MARGIN = MARGIN;

const int LABEL_WIDTH = resolve(Size::S);
const int INPUT_WIDTH = resolve(Size::M);
const int PRICE_WIDTH = resolve(Size::M);

const int V_SPACER = resolve(Spacing::XS);
const int H_SPACER = resolve(Spacing::XS);

auto margin(QWidget *widget) -> QWidget *;
auto margin(QWidget *widget, int margin) -> QWidget *;
// Wraps `widget` in a silent-design DashboardLayout. `two_column` marks a screen
// that can split side by side (Send); single-column screens leave it false.
auto dashboard(const QString &title, QWidget *widget, bool two_column = false,
               QWidget *title_right = nullptr) -> QWidget *;
// Swap a screen's content widget under a stable outer layout: deletes the old
// `main_widget` (and its whole subtree) and installs `content` in its place.
void setScreenContent(QWidget *screen, QWidget *&main_widget, QWidget *content);
auto frame(QWidget *widget) -> QWidget *;
auto shortenOutpoint(const QString &outpoint) -> QString;
auto coinsCount(uint64_t count) -> QString;
// What a failure no kind tells more about is summed up as.
auto operationFailedSummary() -> QString;
auto formatBackendErrorDetails(const QString &raw_error) -> QString;
