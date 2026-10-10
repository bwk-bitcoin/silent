#pragma once

#include <QIcon>

// Resolved against the live palette at paint time, so an icon both follows the
// theme and can be tinted per use site (accent / secondary / muted).
enum class IconColor { Default, Accent, Secondary, Muted, Success, Warning, Error, OnAccent };

// An icon helper below, so a widget can pick the tint itself.
using IconFn = QIcon (*)(IconColor);

// Builds a QIcon that re-renders the SVG with the chosen palette color on every
// paint, so icons follow theme (light/dark) changes without recreation.
auto renderIcon(const char *svg_data, int stroke_width = 2, IconColor color = IconColor::Default)
    -> QIcon;

namespace icon {

auto trash(IconColor color = IconColor::Default) -> QIcon;
auto close(IconColor color = IconColor::Default) -> QIcon;
auto copy(IconColor color = IconColor::Default) -> QIcon;
auto plus(IconColor color = IconColor::Default) -> QIcon;
auto minus(IconColor color = IconColor::Default) -> QIcon;
auto download(IconColor color = IconColor::Default) -> QIcon;
auto upload(IconColor color = IconColor::Default) -> QIcon;
auto chevronLeft(IconColor color = IconColor::Default) -> QIcon;
auto chevronRight(IconColor color = IconColor::Default) -> QIcon;
auto chevronDown(IconColor color = IconColor::Default) -> QIcon;
auto check(IconColor color = IconColor::Default) -> QIcon;
auto x(IconColor color = IconColor::Default) -> QIcon;
auto network(IconColor color = IconColor::Default) -> QIcon;
auto eyeOff(IconColor color = IconColor::Default) -> QIcon;
auto coins(IconColor color = IconColor::Default) -> QIcon;
auto send(IconColor color = IconColor::Default) -> QIcon;
auto receive(IconColor color = IconColor::Default) -> QIcon;
auto settings(IconColor color = IconColor::Default) -> QIcon;
auto pencil(IconColor color = IconColor::Default) -> QIcon;
auto history(IconColor color = IconColor::Default) -> QIcon;
auto folder(IconColor color = IconColor::Default) -> QIcon;
auto usb(IconColor color = IconColor::Default) -> QIcon;
auto refresh(IconColor color = IconColor::Default) -> QIcon;
auto qr(IconColor color = IconColor::Default) -> QIcon;
auto layout(IconColor color = IconColor::Default) -> QIcon;
auto silentMark(IconColor color = IconColor::Default) -> QIcon;
auto key(IconColor color = IconColor::Default) -> QIcon;
auto lock(IconColor color = IconColor::Default) -> QIcon;
auto sdCard(IconColor color = IconColor::Default) -> QIcon;
auto arrowUp(IconColor color = IconColor::Default) -> QIcon;
auto arrowDown(IconColor color = IconColor::Default) -> QIcon;
auto eye(IconColor color = IconColor::Default) -> QIcon;
auto alert(IconColor color = IconColor::Default) -> QIcon;
auto info(IconColor color = IconColor::Default) -> QIcon;
auto circleCheck(IconColor color = IconColor::Default) -> QIcon;
auto circleX(IconColor color = IconColor::Default) -> QIcon;
auto puzzle(IconColor color = IconColor::Default) -> QIcon;

} // namespace icon
