#include "Theme.h"
#include "Icon.h"
#include "catalog/BalanceHeader.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Table.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/containers/Separator.h"
#include "catalog/containers/Tab.h"
#include "catalog/display/Badge.h"
#include "catalog/display/CopyField.h"
#include "catalog/display/Display.h"
#include "catalog/display/Label.h"
#include "catalog/display/WordGrid.h"
#include "catalog/feedback/Notice.h"
#include "catalog/feedback/ScanProgress.h"
#include "catalog/feedback/Tooltip.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/TextEdit.h"
#include "catalog/inputs/Toggle.h"
#include <QApplication>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>
#include <QScrollBar>
#include <QStyleFactory>
#include <QStyleOption>

namespace {

class AppStyle : public QProxyStyle {
public:
    explicit AppStyle(QStyle *base) : QProxyStyle(base) {
    }

    auto pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                     const QWidget *widget = nullptr) const -> int override {
        // The tab close button holds the cross on its left and the space after it: Qt
        // keeps the button only a few pixels from the tab edge whatever the padding, so
        // the button itself carries the rest of the tab's right padding.
        if (metric == PM_TabCloseIndicatorWidth) {
            return metric::TAB_CLOSE_SIZE + metric::TAB_CLOSE_TRAIL;
        }
        if (metric == PM_TabCloseIndicatorHeight) {
            return metric::TAB_CLOSE_SIZE;
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    auto standardIcon(StandardPixmap sp, const QStyleOption *opt = nullptr,
                      const QWidget *widget = nullptr) const -> QIcon override {
        if (sp == SP_TabCloseButton) {
            return icon::close();
        }
        return QProxyStyle::standardIcon(sp, opt, widget);
    }

    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *painter,
                       const QWidget *widget = nullptr) const override {
        if (pe == PE_IndicatorTabClose) {
            const int icon_size = metric::TAB_CLOSE_SIZE;
            auto ic = standardIcon(SP_TabCloseButton, opt, widget);
            auto pix = ic.pixmap(icon_size, icon_size);
            // On the button's left: the space after it is the tab's padding.
            int x = opt->rect.x();
            int y = opt->rect.y() + (opt->rect.height() - icon_size) / 2;
            painter->drawPixmap(x, y, pix);
            return;
        }
        // The stylesheet styles focus itself: no dotted rectangle on top.
        if (pe == PE_FrameFocusRect) {
            return;
        }
        QProxyStyle::drawPrimitive(pe, opt, painter, widget);
    }

    void polish(QWidget *widget) override {
        QProxyStyle::polish(widget);
        // A scrollbar says it paints every pixel, so Qt paints nothing behind it, but
        // ours leave their track transparent: what shows there is whatever the window
        // buffer held (dark on some setups). Have the background painted under them.
        if (qobject_cast<QScrollBar *>(widget) != nullptr) {
            widget->setAttribute(Qt::WA_OpaquePaintEvent, false);
        }
    }

    using QProxyStyle::polish;
};

} // namespace

Theme *Theme::s_instance = nullptr;

Theme::Theme(QObject *parent) : QObject(parent) {
}

void Theme::init() {
    if (s_instance != nullptr) {
        return;
    }
    s_instance = new Theme(qApp);
    // One known base under the stylesheet on every platform, not whatever style the
    // desktop installs. Windows over Fusion: it follows the palette and does not guess
    // a dark theme from a widget's background (Fusion does since Qt 6.9, and takes our
    // transparent areas for dark ones).
    qApp->setStyle(new AppStyle(QStyleFactory::create("Windows")));
}

auto Theme::get() -> Theme * {
    return s_instance;
}

void Theme::setMode(ThemeMode mode) {
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    m_palette = (mode == ThemeMode::Dark) ? Palette::dark() : Palette::light();
    m_font_palette = (mode == ThemeMode::Dark) ? FontPalette::dark() : FontPalette::light();
    m_button_palette = (mode == ThemeMode::Dark) ? ButtonPalette::dark() : ButtonPalette::light();
    m_icon_palette = (mode == ThemeMode::Dark) ? IconPalette::dark() : IconPalette::light();
    m_input_palette = (mode == ThemeMode::Dark) ? InputPalette::dark() : InputPalette::light();
    apply();
    emit themeChanged(m_mode);
}

void Theme::setPalette(const Palette &palette) {
    m_palette = palette;
}

auto Theme::mode() const -> ThemeMode {
    return m_mode;
}

auto Theme::palette() const -> const Palette & {
    return m_palette;
}

auto Theme::fontPalette() const -> const FontPalette & {
    return m_font_palette;
}

auto Theme::buttonPalette() const -> const ButtonPalette & {
    return m_button_palette;
}

auto Theme::iconPalette() const -> const IconPalette & {
    return m_icon_palette;
}

auto Theme::inputPalette() const -> const InputPalette & {
    return m_input_palette;
}

void Theme::apply() {
    const auto &p = m_palette;

    // Build QPalette: Fusion style respects all these roles
    QPalette pal;

    pal.setColor(QPalette::Window, p.bg);
    pal.setColor(QPalette::WindowText, p.text);
    pal.setColor(QPalette::Base, p.inputBg);
    pal.setColor(QPalette::AlternateBase, p.surface);
    pal.setColor(QPalette::ToolTipBase, p.surface);
    pal.setColor(QPalette::ToolTipText, p.text);
    pal.setColor(QPalette::Text, p.text);
    pal.setColor(QPalette::Button, p.bgSecondary);
    pal.setColor(QPalette::ButtonText, p.text);
    pal.setColor(QPalette::BrightText, p.error);
    pal.setColor(QPalette::Link, p.accent);
    pal.setColor(QPalette::Highlight, p.accent);
    pal.setColor(QPalette::HighlightedText, p.onAccent);
    pal.setColor(QPalette::PlaceholderText, p.textMuted);
    pal.setColor(QPalette::Light, p.surfaceHover);
    pal.setColor(QPalette::Midlight, p.borderLight);
    pal.setColor(QPalette::Mid, p.border);
    pal.setColor(QPalette::Dark, p.bgSecondary);
    pal.setColor(QPalette::Shadow, p.bg);

    // Disabled state
    pal.setColor(QPalette::Disabled, QPalette::WindowText, p.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Text, p.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, p.textMuted);

    qApp->setPalette(pal);

    // Supplementary QSS for elements QPalette doesn't fully cover
    auto qss = QString(
                   // --- Table (flat: header bottom-rule only, no gridlines) ---
                   "QHeaderView::section { background: transparent; color: %1; padding: 4px 8px; "
                   "  border: none; border-bottom: 1px solid %2; }"
                   "QTableWidget { gridline-color: transparent; }"
                   // --- ScrollArea ---
                   "QScrollArea { border: none; }"
                   // --- Tooltip (dark bubble per design) ---
                   "QToolTip { background: %3; color: %4; border: none; padding: 5px 9px; }")
                   .arg(p.textSecondary.name()) // %1
                   .arg(p.border.name())        // %2
                   .arg(p.text.name())          // %3 tooltip bg
                   .arg(p.bg.name());           // %4 tooltip fg

    qss += catalog::Tab::qss(p);
    qss += catalog::Button::qss(p);
    qss += catalog::Label::qss(p);
    qss += catalog::Input::qss(p);
    qss += catalog::Display::qss(p);
    qss += catalog::Checkbox::qss(p);
    qss += catalog::ComboBox::qss(p);
    qss += catalog::TextEdit::qss(p);
    qss += catalog::Separator::qss(p);
    qss += catalog::ScrollArea::qss(p);
    qss += catalog::Card::qss(p);
    qss += catalog::Badge::qss(p);
    qss += catalog::ValidationMark::qss(p);
    qss += catalog::Table::qss(p);
    qss += catalog::SelectRow::qss(p);
    qss += catalog::Tooltip::qss(p);
    qss += catalog::ScanProgress::qss(p);
    qss += catalog::Notice::qss(p);
    qss += catalog::SectionHeader::qss(p);
    qss += catalog::CopyField::qss(p);
    qss += catalog::WordGrid::qss(p);
    qss += catalog::BalanceHeader::qss(p);
    qss += catalog::FoldSection::qss(p);

    qApp->setStyleSheet(qss);
}
