#include "Palette.h"

// ===== Color helpers =====

namespace {

// Off badge text: muted text sunk 40% into the page background.
constexpr qreal BADGE_OFF_BG_RATIO = 0.4;

} // namespace

auto mixColor(const QColor &from, const QColor &to, qreal ratio) -> QColor {
    return QColor::fromRgbF(
        static_cast<float>(from.redF() + ((to.redF() - from.redF()) * ratio)),
        static_cast<float>(from.greenF() + ((to.greenF() - from.greenF()) * ratio)),
        static_cast<float>(from.blueF() + ((to.blueF() - from.blueF()) * ratio)));
}

auto withAlpha(const QColor &color, qreal alpha) -> QColor {
    auto result = color;
    result.setAlphaF(static_cast<float>(alpha));
    return result;
}

auto qssColor(const QColor &color) -> QString {
    return QString("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alphaF(), 0, 'f', 3);
}

// ===== Color Palette =====

auto Palette::dark() -> Palette {
    return Palette{
        // Backgrounds
        .bg = color::GRAY_95,
        .bgSecondary = color::GRAY_90,
        .surface = color::GRAY_85,
        .surfaceHover = color::GRAY_80,

        // Text
        .text = color::GRAY_10,
        .textSecondary = color::GRAY_35,
        .textMuted = color::GRAY_55,

        // Brand / Accent
        .accent = color::BLUE,
        .accentHover = color::BLUE_LIGHT,
        .onAccent = color::WHITE,

        // Semantic
        .success = color::TEAL,
        .error = color::RED,
        .errorHover = QColor(255, 81, 81),
        .warning = color::YELLOW,

        // Transaction flow
        .txSelf = color::TX_SELF,
        .txSelfLabel = color::TX_SELF_LABEL,
        .txFeeLabel = color::TX_FEE_LABEL,
        .txRecipientLabel = color::TX_RECIPIENT_LABEL,
        .txWarning = color::TX_WARNING_DARK,

        // Badges
        .badgeSuccessBg = QColor(47, 194, 176, 36),
        .badgeSuccessBorder = QColor(47, 194, 176, 115),
        .badgeSuccessText = color::TEAL,
        .badgeWarningBg = QColor(220, 180, 50, 41),
        .badgeWarningBorder = QColor(220, 180, 50, 122),
        .badgeWarningText = color::YELLOW,
        .badgeErrorBg = QColor(220, 70, 70, 36),
        .badgeErrorBorder = QColor(220, 70, 70, 115),
        .badgeErrorText = color::RED,
        .badgeNeutralBg = color::GRAY_90,
        .badgeNeutralBorder = color::GRAY_80,
        .badgeNeutralText = color::GRAY_35,
        .badgeOffBg = QColor(Qt::transparent),
        .badgeOffBorder = color::GRAY_80,
        .badgeOffText = mixColor(color::GRAY_55, color::GRAY_95, BADGE_OFF_BG_RATIO),

        // Borders
        .border = color::GRAY_70,
        .borderLight = color::GRAY_80,

        // Inputs
        .inputBg = color::GRAY_90,
        .inputBorder = color::GRAY_70,
        .inputFocus = color::BLUE,
    };
}

auto Palette::light() -> Palette {
    return Palette{
        // Backgrounds
        .bg = color::GRAY_4,
        .bgSecondary = color::GRAY_8,
        .surface = color::WHITE,
        .surfaceHover = color::GRAY_5,

        // Text
        .text = color::BLACK,
        .textSecondary = color::LIGHT_GRAY_65,
        .textMuted = color::GRAY_40,

        // Brand / Accent
        .accent = color::BLUE_DARK,
        .accentHover = color::BLUE_DARK_LIGHT,
        .onAccent = color::WHITE,

        // Semantic
        .success = color::TEAL_DARK,
        .error = color::RED_DARK,
        .errorHover = QColor(240, 60, 60),
        .warning = color::YELLOW_DARK,

        // Transaction flow
        .txSelf = color::TX_SELF,
        .txSelfLabel = color::TX_SELF_LABEL,
        .txFeeLabel = color::TX_FEE_LABEL,
        .txRecipientLabel = color::TX_RECIPIENT_LABEL,
        .txWarning = color::TX_WARNING_LIGHT,

        // Badges
        .badgeSuccessBg = QColor(18, 133, 122, 36),
        .badgeSuccessBorder = QColor(18, 133, 122, 115),
        .badgeSuccessText = color::TEAL_DARK,
        .badgeWarningBg = QColor(200, 160, 30, 41),
        .badgeWarningBorder = QColor(200, 160, 30, 122),
        .badgeWarningText = color::YELLOW_DARK,
        .badgeErrorBg = QColor(200, 50, 50, 36),
        .badgeErrorBorder = QColor(200, 50, 50, 115),
        .badgeErrorText = color::RED_DARK,
        .badgeNeutralBg = color::GRAY_8,
        .badgeNeutralBorder = color::GRAY_15,
        .badgeNeutralText = color::LIGHT_GRAY_65,
        .badgeOffBg = QColor(Qt::transparent),
        .badgeOffBorder = color::GRAY_15,
        .badgeOffText = mixColor(color::GRAY_40, color::GRAY_4, BADGE_OFF_BG_RATIO),

        // Borders
        .border = color::GRAY_20,
        .borderLight = color::GRAY_15,

        // Inputs
        .inputBg = color::WHITE,
        .inputBorder = color::GRAY_20,
        .inputFocus = color::BLUE_DARK,
    };
}

// ===== Typography =====

auto ButtonPalette::defaultButtonPalette() -> ButtonPalette {
    return ButtonPalette{
        .defaultBtn = {.font = {.family = font::DEFAULT,
                                .size = size::BODY,
                                .weight = QFont::Normal}},
        .primary = {.font = {.family = font::DEFAULT, .size = size::BODY, .weight = QFont::Normal}},
        .destructive = {.font = {.family = font::DEFAULT,
                                 .size = size::BODY,
                                 .weight = QFont::Normal}},
        .menu = {.font = {.family = font::DEFAULT, .size = size::H3, .weight = QFont::Normal},
                 .paddingH = 16,
                 .paddingV = 12,
                 .borderRadius = radius::MENU},
        .icon = {.font = {.family = font::DEFAULT, .size = size::BODY, .weight = QFont::Normal},
                 .width = resolve(BtnWidth::IconLg),
                 .height = resolve(BtnWidth::IconLg),
                 .paddingH = 0,
                 .paddingV = 0},
        .inlineIcon = {.font = {.family = font::DEFAULT,
                                .size = size::BODY,
                                .weight = QFont::Normal},
                       .width = resolve(BtnWidth::Icon),
                       .height = resolve(BtnWidth::Icon),
                       .paddingH = 0,
                       .paddingV = 0,
                       .borderRadius = radius::ICON},
        .tabOpen = {.font = {.family = font::DEFAULT, .size = size::H3, .weight = QFont::Normal},
                    .width = resolve(BtnWidth::LaunchEntry),
                    .height = 50},
        .tabCreate = {.font = {.family = font::DEFAULT,
                               .size = size::H3,
                               .weight = QFont::DemiBold},
                      .width = resolve(BtnWidth::LaunchAdd),
                      .height = 50},
        .inlineBtn = {.font = {.family = font::DEFAULT,
                               .size = size::BODY,
                               .weight = QFont::Normal},
                      .paddingH = 10,
                      .paddingV = 4,
                      .borderRadius = 6},
        .index = {.font = {.family = font::MONO, .size = size::CAPTION, .weight = QFont::DemiBold},
                  .width = resolve(BtnWidth::Index),
                  .height = resolve(BtnWidth::Icon),
                  .paddingH = 8,
                  .paddingV = 0},
    };
}

auto ButtonPalette::dark() -> ButtonPalette {
    return defaultButtonPalette();
}

auto ButtonPalette::light() -> ButtonPalette {
    return defaultButtonPalette();
}

auto FontPalette::defaultFontPalette() -> FontPalette {
    return FontPalette{
        .title = {.family = font::DEFAULT, .size = size::H1, .weight = QFont::DemiBold},
        .heading = {.family = font::DEFAULT, .size = size::H2, .weight = QFont::DemiBold},
        .subheading = {.family = font::DEFAULT, .size = size::H3, .weight = QFont::Normal},
        .section = {.family = font::DEFAULT, .size = size::SECTION, .weight = QFont::DemiBold},
        .inputLabel = {.family = font::DEFAULT,
                       .size = size::BODY,
                       .weight = QFont::Normal,
                       .width = resolve(Size::S)},
        .checkboxLabel = {.family = font::DEFAULT,
                          .size = size::CAPTION,
                          .weight = QFont::Normal,
                          .width = resolve(Size::XS)},
        .infoLabel = {.family = font::DEFAULT,
                      .size = size::BODY,
                      .weight = QFont::Normal,
                      .width = resolve(Size::S)},
        .info = {.family = font::DEFAULT, .size = size::BODY, .weight = QFont::Normal},
        .body = {.family = font::DEFAULT, .size = size::BODY, .weight = QFont::Normal},
        .caption = {.family = font::DEFAULT, .size = size::CAPTION, .weight = QFont::Normal},
        .mono = {.family = font::MONO, .size = size::BODY, .weight = QFont::Normal},
        .status = {.family = font::DEFAULT, .size = size::CAPTION, .weight = QFont::Normal},
        .eyebrow = {.family = font::MONO, .size = size::CAPTION, .weight = QFont::DemiBold},
    };
}

auto FontPalette::dark() -> FontPalette {
    return defaultFontPalette();
}

auto FontPalette::light() -> FontPalette {
    return defaultFontPalette();
}

auto InputPalette::defaultInputPalette() -> InputPalette {
    return InputPalette{
        .defaultInput = {.font = {.family = font::DEFAULT,
                                  .size = size::BODY,
                                  .weight = QFont::Normal}},
        .mono = {.font = {.family = font::MONO, .size = size::BODY, .weight = QFont::Normal}},
        .display = {.font = {.family = font::DEFAULT, .size = size::BODY, .weight = QFont::Normal}},
    };
}

auto InputPalette::dark() -> InputPalette {
    return defaultInputPalette();
}

auto InputPalette::light() -> InputPalette {
    return defaultInputPalette();
}

auto IconPalette::defaultIconPalette() -> IconPalette {
    return IconPalette{
        .defaultIcon = {.size = metric::ICON_SIZE, .strokeWidth = metric::ICON_STROKE},
    };
}

auto IconPalette::dark() -> IconPalette {
    return defaultIconPalette();
}

auto IconPalette::light() -> IconPalette {
    return defaultIconPalette();
}
