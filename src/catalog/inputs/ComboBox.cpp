#include "catalog/inputs/ComboBox.h"
#include "resources/icon/chevron_down.h"
#include "theme/Palette.h"
#include "theme/Theme.h"
#include <QAbstractItemView>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QPainter>
#include <QPixmap>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QSvgRenderer>

namespace catalog {

// The plain list popup comes from the app style (SH_ComboBox_Popup): a style of its own
// here would sit on the desktop's native style instead, which draws its focus frame
// around the combobox.
ComboBox::ComboBox(ComboBoxRole role, QWidget *parent) : QComboBox(parent), m_role(role) {
    applyRole();
}

void ComboBox::setRole(ComboBoxRole role) {
    if (m_role == role) {
        return;
    }
    m_role = role;
    applyRole();
    style()->unpolish(this);
    style()->polish(this);
}

auto ComboBox::role() const -> ComboBoxRole {
    return m_role;
}

void ComboBox::applyRole() {
    setProperty("class", "combobox");

    const auto &fp = Theme::get()->fontPalette();
    auto f = font();
    f.setPointSize(fp.body.size);
    f.setFamily(fp.body.family);
    f.setWeight(fp.body.weight);
    setFont(f);
}

void ComboBox::setWidth(Size s) {
    setFixedWidth(resolve(s));
}

void ComboBox::setItemEnabled(int index, bool enabled) {
    // QComboBox's own model: one QStandardItem per entry.
    auto *items = qobject_cast<QStandardItemModel *>(model());
    if (items == nullptr || items->item(index) == nullptr) {
        return;
    }
    items->item(index)->setEnabled(enabled);
}

void ComboBox::showPopup() {
    // Restyle the container before showing it: changing a window flag on a visible
    // window re-parents it, and that hides it, so doing this after the base class
    // swallowed the first click on every combobox. view() creates the container
    // lazily, and it is still hidden here, so the flags apply without a hide.
    auto *container = view()->parentWidget();
    if (container != nullptr) {
        container->setWindowFlag(Qt::FramelessWindowHint);
        container->setAttribute(Qt::WA_TranslucentBackground);
        container->setContentsMargins(0, 0, 0, 0);
        if (auto *frame = qobject_cast<QFrame *>(container)) {
            frame->setFrameShape(QFrame::NoFrame);
        }
        // No mask: a 1-bit mask aliases the rounded corners. The translucent window lets the
        // view's QSS border-radius round them with anti-aliasing instead.
    }

    QComboBox::showPopup();
}

auto ComboBox::renderChevron(const QColor &color) -> QString {
    // QSS `image: url(...)` needs a file path, and Qt caches the loaded pixmap by
    // that URL. Encode the color in the filename so a theme change resolves to a
    // new URL instead of silently reusing the cached chevron of the old color.
    auto dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    auto path = dir + QString("/silent_chevron_down_%1.png").arg(color.name().mid(1));
    if (QFile::exists(path)) {
        return path;
    }

    auto svg = QString(embedded_icon::CHEVRON_DOWN);
    svg.replace("stroke=\"currentColor\"", QString("stroke=\"%1\"").arg(color.name()));

    constexpr int size = 12;
    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    pixmap.save(path);
    return path;
}

auto ComboBox::qss(const Palette &p) -> QString {
    auto chevron = renderChevron(p.text);
    auto disabledChevron = renderChevron(p.textSecondary);

    return QString("QComboBox[class=\"combobox\"] {"
                   "  border: 1px solid %1;"
                   "  background: %2;"
                   "  border-radius: %8px;"
                   "  padding: %9px %10px %9px %11px;"
                   "  color: %3;"
                   "}"
                   "QComboBox[class=\"combobox\"]:focus {"
                   "  border: 1px solid %4;"
                   "}"
                   "QComboBox[class=\"combobox\"]:disabled {"
                   "  background: %5;"
                   "  color: %14;"
                   "  border-color: %15;"
                   "}"
                   "QComboBox[class=\"combobox\"]::drop-down {"
                   "  border: none;"
                   "  border-left: 1px solid %1;"
                   "  background: %5;"
                   "  border-top-right-radius: %8px;"
                   "  border-bottom-right-radius: %8px;"
                   "  width: %12px;"
                   "}"
                   "QComboBox[class=\"combobox\"]::drop-down:hover {"
                   "  background: %6;"
                   "}"
                   "QComboBox[class=\"combobox\"]::down-arrow {"
                   "  image: url(%7);"
                   "  width: %13px;"
                   "  height: %13px;"
                   "}"
                   "QComboBox[class=\"combobox\"]::drop-down:disabled {"
                   "  border-left-color: %15;"
                   "}"
                   "QComboBox[class=\"combobox\"]::down-arrow:disabled {"
                   "  image: url(%16);"
                   "}"
                   "QComboBox[class=\"combobox\"] QAbstractItemView {"
                   "  background: %2;"
                   "  color: %3;"
                   "  border: 1px solid %1;"
                   "  border-radius: %8px;"
                   "  outline: none;"
                   // Rows inset and rounded: a highlighted first or last row stays
                   // inside the popup's rounded corners.
                   "  padding: %18px;"
                   "  selection-background-color: %6;"
                   "  selection-color: %3;"
                   "}"
                   // Rows as the kit draws them: the highlighted one keeps the text
                   // colour on the hover background.
                   "QComboBox[class=\"combobox\"] QAbstractItemView::item {"
                   "  padding: %9px %11px;"
                   "  border: none;"
                   "  border-radius: %19px;"
                   "}"
                   "QComboBox[class=\"combobox\"] QAbstractItemView::item:selected {"
                   "  background: %6;"
                   "  color: %3;"
                   "}"
                   // A disabled entry goes muted, as a disabled row does.
                   "QComboBox[class=\"combobox\"] QAbstractItemView::item:disabled {"
                   "  background: transparent;"
                   "  color: %17;"
                   "}")
        .arg(p.inputBorder.name())                               // %1
        .arg(p.inputBg.name())                                   // %2
        .arg(p.text.name())                                      // %3
        .arg(p.inputFocus.name())                                // %4
        .arg(p.bgSecondary.name())                               // %5
        .arg(p.surfaceHover.name())                              // %6
        .arg(chevron)                                            // %7
        .arg(radius::COMBO)                                      // %8
        .arg(resolve(Padding::XS))                               // %9
        .arg(metric::COMBO_DROPDOWN_WIDTH + resolve(Padding::S)) // %10
        .arg(resolve(Padding::S))                                // %11
        .arg(metric::COMBO_DROPDOWN_WIDTH)                       // %12
        .arg(metric::COMBO_ARROW_SIZE)                           // %13
        .arg(p.textSecondary.name())                             // %14
        .arg(p.borderLight.name())                               // %15
        .arg(disabledChevron)                                    // %16
        .arg(p.textMuted.name())                                 // %17
        .arg(resolve(Padding::XXS))                              // %18
        .arg(radius::INPUT);                                     // %19
}

} // namespace catalog
