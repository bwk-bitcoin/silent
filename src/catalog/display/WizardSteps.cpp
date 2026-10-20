#include "catalog/display/WizardSteps.h"

#include "catalog/display/Label.h"
#include "theme/Icon.h"
#include "theme/Theme.h"

#include <QLabel>
#include <QStyle>
#include <Qontrol>
#include <algorithm>

namespace catalog {

namespace {

const int DOT_SIZE = 20;
const int CHECK_SIZE = 12;
// Six steps fit the wizard header, regardless of label language.
const int NUMBER_PITCH = 120;
const int MAX_NAME_WIDTH = 70;
const int MAX_TRAIL_WIDTH = 695;

void sizeName(Label *name, int max_width) {
    name->ensurePolished();
    auto nameFont = name->font();
    auto normalSize = name->sizeHint();
    nameFont.setWeight(font::DEMIBOLD);
    name->setFont(nameFont);
    auto boldSize = name->sizeHint();
    auto width = std::max(normalSize.width(), boldSize.width());
    auto height = std::max(normalSize.height(), boldSize.height());
    if (width > max_width) {
        name->setWordWrap(true);
        if (name->minimumSizeHint().width() > max_width) {
            // Even an unbroken translated word must stay inside its number interval.
            name->setTextWrapAnywhere(name->text());
        }
        width = max_width;
        height = name->heightForWidth(width);
        nameFont.setWeight(QFont::Normal);
        name->setFont(nameFont);
        height = std::max(height, name->heightForWidth(width));
    }
    name->setFixedSize(width, height);
}

void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

} // namespace

WizardSteps::WizardSteps(const QStringList &steps, QWidget *parent) : QWidget(parent) {
    setProperty("class", "wizard-steps");
    setAttribute(Qt::WA_StyledBackground, true);
    // Centred across the modal.
    auto gap = resolve(Spacing::XS);
    auto *row = (new qontrol::Row)->spacing(0)->pushSpacer();
    for (int i = 0; i < steps.size(); ++i) {
        auto *dot = new QLabel(this);
        dot->setProperty("class", "wizard-dot");
        dot->setFixedSize(DOT_SIZE, DOT_SIZE);
        dot->setAlignment(Qt::AlignCenter);
        auto dotFont = dot->font();
        dotFont.setFamily(Theme::get()->fontPalette().mono.family);
        dotFont.setPointSize(Theme::get()->fontPalette().caption.size);
        dotFont.setWeight(font::DEMIBOLD);
        dot->setFont(dotFont);
        auto *name = new Label(steps.at(i), LabelRole::Caption, this);
        bool last = i == steps.size() - 1;
        auto nameWidth =
            last ? std::max(MAX_NAME_WIDTH, MAX_TRAIL_WIDTH - i * NUMBER_PITCH - DOT_SIZE - gap)
                 : MAX_NAME_WIDTH;
        sizeName(name, nameWidth);
        m_dots.append(dot);
        m_names.append(name);
        auto *step = (new qontrol::Row)->spacing(gap)->push(dot)->push(name);
        if (!last) {
            auto *rule = new QWidget(this);
            rule->setProperty("class", "wizard-rule");
            rule->setAttribute(Qt::WA_StyledBackground, true);
            rule->setFixedHeight(1);
            rule->setMinimumWidth(gap);
            m_rules.append(rule);
            step->push(rule, 1)->margins(0, 0, gap, 0);
            step->setFixedWidth(NUMBER_PITCH);
        } else {
            step->setFixedWidth(DOT_SIZE + gap + name->width());
        }
        row->push(step);
    }
    row->pushSpacer();
    (new qontrol::Column)->margins(0, 0, 0, resolve(Spacing::S))->push(row)->into(this);
    refresh();
}

void WizardSteps::setCurrent(int index) {
    m_current = index;
    refresh();
}

void WizardSteps::refresh() {
    for (int i = 0; i < m_dots.size(); ++i) {
        auto *dot = m_dots.at(i);
        auto *name = m_names.at(i);
        bool done = i < m_current;
        bool current = i == m_current;
        if (done) {
            dot->setProperty("state", "done");
            dot->setText({});
            dot->setPixmap(icon::check(IconColor::OnAccent).pixmap(CHECK_SIZE, CHECK_SIZE));
            name->setTone(LabelTone::Secondary);
        } else {
            dot->setProperty("state", current ? "current" : "next");
            dot->setPixmap({});
            dot->setText(QString::number(i + 1));
            name->setTone(current ? LabelTone::Default : LabelTone::Muted);
        }
        auto nameFont = name->font();
        nameFont.setWeight(current ? font::DEMIBOLD : QFont::Normal);
        name->setFont(nameFont);
        repolish(dot);
    }
    for (int i = 0; i < m_rules.size(); ++i) {
        // The rule before step i + 1 is done once that step is reached.
        m_rules.at(i)->setProperty("done", i + 1 <= m_current);
        repolish(m_rules.at(i));
    }
}

auto WizardSteps::qss(const Palette &p) -> QString {
    return QString("QWidget[class=\"wizard-steps\"] { border-bottom: 1px solid %1; }"
                   "QWidget[class=\"wizard-rule\"] { background: %1; }"
                   "QWidget[class=\"wizard-rule\"][done=\"true\"] { background: %2; }"
                   "QLabel[class=\"wizard-dot\"] { border: 1px solid %3; border-radius: %4px;"
                   "  color: %5; background: transparent; }"
                   "QLabel[class=\"wizard-dot\"][state=\"current\"] { border-color: %2;"
                   "  color: %2; }"
                   "QLabel[class=\"wizard-dot\"][state=\"done\"] { border-color: %2;"
                   "  background: %2; }")
        .arg(p.borderLight.name())
        .arg(p.accent.name())
        .arg(p.border.name())
        .arg(DOT_SIZE / 2)
        .arg(p.textMuted.name());
}

} // namespace catalog
