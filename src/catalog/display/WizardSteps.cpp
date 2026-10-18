#include "catalog/display/WizardSteps.h"

#include "catalog/display/Label.h"
#include "theme/Icon.h"
#include "theme/Theme.h"

#include <QLabel>
#include <QStyle>
#include <Qontrol>

namespace catalog {

namespace {

const int DOT_SIZE = 20;
const int CHECK_SIZE = 12;
// The fixed rule between two steps.
const int RULE_WIDTH = 40;

void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

} // namespace

WizardSteps::WizardSteps(const QStringList &steps, QWidget *parent) : QWidget(parent) {
    setProperty("class", "wizard-steps");
    setAttribute(Qt::WA_StyledBackground, true);
    // Centred across the modal.
    auto *row = (new qontrol::Row)->spacing(resolve(Spacing::XS))->pushSpacer();
    for (int i = 0; i < steps.size(); ++i) {
        if (i > 0) {
            auto *rule = new QWidget(this);
            rule->setProperty("class", "wizard-rule");
            rule->setAttribute(Qt::WA_StyledBackground, true);
            rule->setFixedSize(RULE_WIDTH, 1);
            m_rules.append(rule);
            row->push(rule);
        }
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
        m_dots.append(dot);
        m_names.append(name);
        row->push((new qontrol::Row)->spacing(resolve(Spacing::XS))->push(dot)->push(name));
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
