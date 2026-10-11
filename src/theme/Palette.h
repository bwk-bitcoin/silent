#pragma once

#include <QColor>
#include <QFont>
#include <QString>

// ===== Color Constants =====

namespace color {

// Neutrals: dark
const QColor GRAY_95 = QColor(30, 30, 30);
const QColor GRAY_90 = QColor(40, 40, 40);
const QColor GRAY_85 = QColor(50, 50, 50);
const QColor GRAY_80 = QColor(60, 60, 60);
const QColor GRAY_70 = QColor(80, 80, 80);
const QColor GRAY_55 = QColor(110, 110, 110);
const QColor GRAY_40 = QColor(150, 150, 150);
const QColor GRAY_35 = QColor(170, 170, 170);
const QColor GRAY_20 = QColor(190, 190, 190);
const QColor GRAY_15 = QColor(210, 210, 210);
const QColor GRAY_10 = QColor(230, 230, 230);
const QColor GRAY_8 = QColor(235, 235, 235);
const QColor GRAY_5 = QColor(240, 240, 240);
const QColor GRAY_4 = QColor(245, 245, 245);

// Pure
const QColor WHITE = QColor(255, 255, 255);
const QColor BLACK = QColor(30, 30, 30);

// Neutrals: light
const QColor LIGHT_GRAY_65 = QColor(90, 90, 90);

// Blue
const QColor BLUE = QColor(80, 140, 220);
const QColor BLUE_LIGHT = QColor(100, 160, 240);
const QColor BLUE_DARK = QColor(50, 120, 200);
const QColor BLUE_DARK_LIGHT = QColor(70, 140, 220);

// Green
const QColor GREEN = QColor(80, 190, 80);
const QColor GREEN_DARK = QColor(40, 160, 40);

// Teal (success)
const QColor TEAL = QColor(0x2f, 0xc2, 0xb0);
const QColor TEAL_DARK = QColor(0x12, 0x85, 0x7a);

// Red
const QColor RED = QColor(220, 70, 70);
const QColor RED_DARK = QColor(200, 50, 50);

// Yellow
const QColor YELLOW = QColor(220, 180, 50);
const QColor YELLOW_DARK = QColor(200, 160, 30);

// Camera viewport: the same dark surface in both themes
const QColor CAMERA_BG = QColor(0x0c, 0x0e, 0x13);

// Transaction flow
const QColor TX_SELF = QColor(0x3d, 0x8b, 0xf0);
const QColor TX_SELF_LABEL = QColor(0x1f, 0x5f, 0xb0);
const QColor TX_FEE_LABEL = QColor(0xa8, 0x28, 0x28);
const QColor TX_RECIPIENT_LABEL = QColor(0x8a, 0x6d, 0x00);
const QColor TX_WARNING_DARK = QColor(0xff, 0xff, 0x4a);
const QColor TX_WARNING_LIGHT = QColor(0xe6, 0xe6, 0x00);

} // namespace color

// ===== Size Constants =====

namespace size {

const int H1 = 22;
const int H2 = 16;
const int H3 = 14;
const int H4 = 12;
const int SECTION = 11;
const int BODY = 10;
const int CAPTION = 8;

} // namespace size

// ===== Radius Constants =====

namespace radius {

const int INPUT = 6;
const int BUTTON = 8;
const int MENU = 8;
const int COMBO = 8;
const int ICON = 4;
const int TAB = 4;
const int CHECKBOX = 4;
const int QR_CARD = 12;

} // namespace radius

// ===== Metric Constants =====

namespace metric {

const int SIDEBAR_WIDTH = 200;
const int STATUS_BAR_HEIGHT = 30;
const int CHECKBOX_BOX = 18;
const int TOGGLE_WIDTH = 40;
const int TOGGLE_HEIGHT = 20;
const int VALIDATION_MARK_WIDTH = 20;
const int ICON_SIZE = 24;
const int ICON_STROKE = 2;
const int BUTTON_MIN_HEIGHT = 36;
const int COMBO_DROPDOWN_WIDTH = 30;
const int COMBO_ARROW_SIZE = 12;
const int SELECT_ROW_ICON_SIZE = 18;
const int SELECT_ROW_PADDING_H = 14;
const int SELECT_ROW_PADDING_V = 12;
const int MODAL_STATUS_ICON_SIZE = 40;
const int SPINNER_SIZE = 114;
const int TABLE_ROW_HEIGHT = 34;
const int TABLE_CELL_PAD = 7;
const int BADGE_PADDING_H = 11;
const int BADGE_PADDING_V = 5;
const int BADGE_ICON_SIZE = 14;
const int ROUTE_ICON_SIZE = 16;
const int SCAN_PROGRESS_HEIGHT = 6;
const int CHEVRON_SIZE = 16;
const int NOTICE_ICON_SIZE = 16;
const int ACTIONS_REASON_MAX_WIDTH = 260;
const int FIELD_NOTE_ICON_SIZE = 14;
const int FOLD_CHEVRON_SIZE = 14;
const int EMPTY_STATE_ICON_SIZE = 28;
const int MODAL_STATUS_TEXT_MAX_WIDTH = 320;
const int WORD_INDEX_WIDTH = 18;
const int STEPPER_DOTS_WIDTH = 56;
const int ADDRESS_NOTE_MAX_WIDTH = 520;
const int PASSWORD_INPUT_WIDTH = 260;
const int CAMERA_SOURCE_WIDTH = 220;
const int QR_REBUILD_SPINNER_SIZE = 28;
const int CAMERA_STATE_ICON_SIZE = 40;
// The close cross of a tab, as the design's 12px icon.
const int TAB_CLOSE_SIZE = 12;
// Space after the cross inside its button: with the few pixels Qt keeps from the
// tab edge, the cross ends a tab's right padding (Padding::M) from it.
const int TAB_CLOSE_TRAIL = 10;
const int QR_DISPLAY_SIZE = 400;
const int QR_DISPLAY_PADDING = 16;
// A plain request code (an xpub request), without the airgap tuning.
const int QR_REQUEST_SIZE = 360;
const int QR_REQUEST_PADDING = 12;
// Fits the body of the narrowest modal showing a QR request (ModalWidth::L).
const int QR_HINT_MAX_WIDTH = 520;
// The Keys tab is a fixed column, centred.
const int KEYS_TAB_WIDTH = 700;
// A list row's pill, gap and glyph, so the Devices and Keys lists line up.
const int LIST_STATUS_WIDTH = 146;
const int ROW_SPINNER_SIZE = 14;
const int ACCOUNT_INDEX_INPUT_WIDTH = 44;
// Key detail table columns.
const int ROUTE_ICON_COLUMN = 40;
const int ROUTE_ACTIONS_COLUMN = 116;
const int XPUB_PATH_COLUMN = 120;
const int XPUB_PILL_COLUMN = 130;
const int ACCOUNT_MODULE_COLUMN = 160;
const int ACCOUNT_PILL_COLUMN = 140;
const int DROP_ICON_SIZE = 48;

} // namespace metric

// ===== Font Constants =====

namespace font {

const QString DEFAULT = "Noto Sans";
const QString MONO = "Noto Sans Mono";

// The design's demibold (600) is drawn with the Noto Sans Bold face: Noto ships no
// static SemiBold there, so a system SemiBold would render lighter than the design.
const QFont::Weight DEMIBOLD = QFont::Bold;

} // namespace font

// ===== Size (widget widths) =====

enum class Size { XXS, XS, S, M, L, XL, XXL, XXXL };

constexpr auto resolve(Size s) -> int {
    switch (s) {
    case Size::XXS:
        return 50;
    case Size::XS:
        return 100;
    case Size::S:
        return 150;
    case Size::M:
        return 200;
    case Size::L:
        return 300;
    case Size::XL:
        return 400;
    case Size::XXL:
        return 600;
    case Size::XXXL:
        return 1000;
    }
    return 200;
}

// ===== Spacing =====

enum class Spacing { XXS, XS, S, M, L, XL, XXL };

constexpr auto resolve(Spacing s) -> int {
    switch (s) {
    case Spacing::XXS:
        return 2;
    case Spacing::XS:
        return 5;
    case Spacing::S:
        return 10;
    case Spacing::M:
        return 20;
    case Spacing::L:
        return 30;
    case Spacing::XL:
        return 60;
    case Spacing::XXL:
        return 120;
    }
    return 20;
}

// ===== Padding =====

enum class Padding { XXS, XS, S, M, L, XL, XXL };

constexpr auto resolve(Padding p) -> int {
    switch (p) {
    case Padding::XXS:
        return 2;
    case Padding::XS:
        return 6;
    case Padding::S:
        return 10;
    case Padding::M:
        return 15;
    case Padding::L:
        return 20;
    case Padding::XL:
        return 30;
    case Padding::XXL:
        return 50;
    }
    return 20;
}

// ===== Width sentinel =====

namespace width {
const int NONE = -1;
} // namespace width

// ===== Button widths (the only ones a button takes; Auto sizes to content) =====

enum class BtnWidth { Icon, IconLg, Index, M, L, XL, XXL, LaunchEntry, LaunchAdd, Auto };

constexpr auto resolve(BtnWidth w) -> int {
    switch (w) {
    case BtnWidth::Icon:
        return 30;
    case BtnWidth::IconLg:
        return 50;
    case BtnWidth::Index:
        return 60;
    case BtnWidth::M:
        return 120;
    case BtnWidth::L:
        return 160;
    case BtnWidth::XL:
        return 200;
    case BtnWidth::XXL:
        return 260;
    case BtnWidth::LaunchEntry:
        return 300;
    case BtnWidth::LaunchAdd:
        return 354;
    case BtnWidth::Auto:
        return width::NONE;
    }
    return width::NONE;
}

// ===== Pill widths (Auto only for data labels) =====

enum class PillWidth { M, L, Auto };

constexpr auto resolve(PillWidth w) -> int {
    switch (w) {
    case PillWidth::M:
        return 110;
    case PillWidth::L:
        return 140;
    case PillWidth::Auto:
        return width::NONE;
    }
    return width::NONE;
}

// ===== Modal widths =====

enum class ModalWidth { XXS, XS, S, M, L, XL, XXL, Wide };

constexpr auto resolve(ModalWidth w) -> int {
    switch (w) {
    case ModalWidth::XXS:
        return 420;
    case ModalWidth::XS:
        return 460;
    case ModalWidth::S:
        return 520;
    case ModalWidth::M:
        return 560;
    case ModalWidth::L:
        return 600;
    case ModalWidth::XL:
        return 620;
    case ModalWidth::XXL:
        return 640;
    case ModalWidth::Wide:
        return 1040;
    }
    return 520;
}

// ===== Color Palette =====

struct Palette {
    // Backgrounds
    QColor bg;
    QColor bgSecondary;
    QColor surface;
    QColor surfaceHover;

    // Text
    QColor text;
    QColor textSecondary;
    QColor textMuted;

    // Brand / Accent
    QColor accent;
    QColor accentHover;
    QColor onAccent;

    // Semantic
    QColor success;
    QColor error;
    QColor errorHover;
    QColor warning;

    // Transaction flow
    QColor txSelf;
    QColor txSelfLabel;
    QColor txFeeLabel;
    QColor txRecipientLabel;
    QColor txWarning;

    // Badges
    QColor badgeSuccessBg;
    QColor badgeSuccessBorder;
    QColor badgeSuccessText;
    QColor badgeWarningBg;
    QColor badgeWarningBorder;
    QColor badgeWarningText;
    QColor badgeErrorBg;
    QColor badgeErrorBorder;
    QColor badgeErrorText;
    QColor badgeNeutralBg;
    QColor badgeNeutralBorder;
    QColor badgeNeutralText;
    QColor badgeOffBg;
    QColor badgeOffBorder;
    QColor badgeOffText;

    // Borders
    QColor border;
    QColor borderLight;

    // Inputs
    QColor inputBg;
    QColor inputBorder;
    QColor inputFocus;

    static auto light() -> Palette;
    static auto dark() -> Palette;
};

namespace theme {
using Palette = ::Palette;
} // namespace theme

// ===== Color helpers =====

// `from` blended toward `to`: ratio 0 keeps `from`, 1 gives `to` (CSS color-mix).
auto mixColor(const QColor &from, const QColor &to, qreal ratio) -> QColor;
// `color` at the given opacity (0..1).
auto withAlpha(const QColor &color, qreal alpha) -> QColor;
// rgba() notation, so QSS keeps the alpha channel.
auto qssColor(const QColor &color) -> QString;

// ===== Typography =====

struct Font {
    QString family;
    int size;
    QFont::Weight weight;
    int width = width::NONE;
};

struct ButtonStyle {
    Font font;
    int width = width::NONE;
    int height = width::NONE;
    int paddingH = 16;
    int paddingV = 6;
    int borderRadius = 8;
};

struct ButtonPalette {
    ButtonStyle defaultBtn;
    ButtonStyle primary;
    ButtonStyle destructive;
    ButtonStyle menu;
    ButtonStyle icon;
    ButtonStyle inlineIcon;
    ButtonStyle tabOpen;
    ButtonStyle tabCreate;
    ButtonStyle inlineBtn;
    ButtonStyle index;

    static auto defaultButtonPalette() -> ButtonPalette;
    static auto dark() -> ButtonPalette;
    static auto light() -> ButtonPalette;
};

struct FontPalette {
    Font title;
    Font heading;
    Font subheading;
    Font section;
    Font inputLabel;
    Font checkboxLabel;
    Font infoLabel;
    Font info;
    Font body;
    Font caption;
    Font mono;
    Font status;
    Font eyebrow;

    static auto defaultFontPalette() -> FontPalette;
    static auto dark() -> FontPalette;
    static auto light() -> FontPalette;
};

struct IconStyle {
    int size;
    int strokeWidth;
};

struct InputStyle {
    Font font;
};

struct InputPalette {
    InputStyle defaultInput;
    InputStyle mono;
    InputStyle display;

    static auto defaultInputPalette() -> InputPalette;
    static auto dark() -> InputPalette;
    static auto light() -> InputPalette;
};

struct IconPalette {
    IconStyle defaultIcon;

    static auto defaultIconPalette() -> IconPalette;
    static auto dark() -> IconPalette;
    static auto light() -> IconPalette;
};
