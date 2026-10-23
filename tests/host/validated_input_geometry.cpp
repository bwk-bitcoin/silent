#include "PriceProbe.h"

#include "account/views/Send.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/form/WordInputGrid.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QLayout>
#include <QMargins>
#include <QPalette>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QWidget>
#include <Qontrol>

namespace smoke {
namespace {

void require(bool passed, const char *context) {
    if (!passed) {
        qFatal("validated input geometry: %s", context);
    }
}

void settle(QWidget &widget) {
    for (int pass = 0; pass < 4; ++pass) {
        for (auto *layout : widget.findChildren<QLayout *>()) {
            layout->activate();
        }
        QApplication::processEvents();
    }
}

auto mark(catalog::ValidatedInput &field) -> catalog::ValidationMark * {
    auto marks = field.input()->findChildren<catalog::ValidationMark *>();
    require(marks.size() == 1, "exactly one integrated mark per input");
    return marks.first();
}

void checkGeometry(catalog::ValidatedInput &field) {
    auto *input = field.input();
    auto *indicator = mark(field);
    require(indicator->parentWidget() == input, "mark belongs to the actual input");
    require(input->rect().contains(indicator->geometry()), "mark remains inside input");
    require(indicator->width() == 20, "mark keeps its 20px width");
    require(indicator->x() == input->width() - 25, "mark tracks input width, not wrapper");
    require(indicator->height() == input->height(), "mark tracks input height");
    require(indicator->testAttribute(Qt::WA_TransparentForMouseEvents),
            "mark must not intercept input clicks");
    require(!indicator->isHidden(), "None clears the mark without hiding it");
    require(input->textMargins() == QMargins(0, 0, 25, 0), "constant 25px extra text gutter");
    QStyleOptionFrame option;
    option.initFrom(input);
    option.lineWidth = input->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, input);
    auto textRect = input->style()->subElementRect(QStyle::SE_LineEditContents, &option, input);
    textRect = textRect.marginsRemoved(input->textMargins());
    require(textRect.right() < indicator->x(), "styled text area ends before mark");
}

void checkStates(catalog::ValidatedInput &field) {
    auto inputGeometry = field.input()->geometry();
    auto fieldGeometry = field.geometry();
    for (auto state : {catalog::ValidationMark::State::None, catalog::ValidationMark::State::Valid,
                       catalog::ValidationMark::State::Invalid}) {
        field.setState(state);
        require(mark(field)->state() == state, "wrapper preserves validation states");
        if (state != catalog::ValidationMark::State::None) {
            auto expectedColor = state == catalog::ValidationMark::State::Valid
                                     ? Theme::get()->palette().success
                                     : Theme::get()->palette().error;
            require(mark(field)->palette().color(QPalette::WindowText) == expectedColor,
                    "state changes update the mark color");
        }
        require(field.input()->geometry() == inputGeometry && field.geometry() == fieldGeometry,
                "validation states do not move the input or wrapper");
        checkGeometry(field);
    }
}

void fixedAndExpanding() {
    QWidget window;
    auto *fixed100 = new catalog::ValidatedInput(catalog::InputRole::Mono);
    fixed100->setWidth(Size::XS);
    auto *fixed150 = new catalog::ValidatedInput;
    fixed150->setWidth(Size::S);
    auto *expanding = new catalog::ValidatedInput(catalog::InputRole::Mono);
    (new qontrol::Column)
        ->push(fixed100)
        ->push(fixed150)
        ->push(expanding)
        ->pushSpacer()
        ->into(&window);
    for (auto *field : {fixed100, fixed150, expanding}) {
        field->input()->setText(QString(300, 'W'));
    }
    for (auto width : {600, 320, 900, 400}) {
        window.resize(width, 300);
        window.show();
        settle(window);
        require(fixed100->width() > 100 && fixed150->width() > 150,
                "fixed inputs are exercised in wider wrappers");
        require(fixed100->input()->width() == 100 && fixed150->input()->width() == 150,
                "fixed input widths stay unchanged");
        require(expanding->input()->width() == expanding->width(), "expanding input fills wrapper");
        for (auto *field : {fixed100, fixed150, expanding}) {
            checkStates(*field);
            field->input()->setCursorPosition(0);
            checkGeometry(*field);
            field->input()->setCursorPosition(field->input()->text().size());
            checkGeometry(*field);
        }
        window.hide();
    }
    window.show();
    settle(window);
    // Resize only the inner input: there need not be a wrapper resize event.
    fixed100->input()->setFixedSize(120, 60);
    settle(window);
    checkStates(*fixed100);
}

auto wordValid(const QString &word) -> bool {
    return word == QStringLiteral("abandon");
}

void wordGrids() {
    QWidget window;
    auto *grid = new catalog::WordInputGrid;
    grid->setWordCheck(wordValid);
    (new qontrol::Column)->push(grid)->pushSpacer()->into(&window);
    for (auto count : {12, 15, 18, 24, 12}) {
        grid->setWordCount(count);
        for (auto width : {700, 1024, 720}) {
            window.resize(width, 768);
            window.show();
            settle(window);
            auto cells = grid->findChildren<catalog::ValidatedInput *>();
            require(cells.size() == count, "mnemonic grid keeps every word input");
            for (auto *cell : cells) {
                require(cell->property("wordIndex").isValid(), "mnemonic cell has word index");
                cell->input()->setText(QStringLiteral("abandon"));
                require(mark(*cell)->state() == catalog::ValidationMark::State::Valid,
                        "dictionary word validates");
                checkGeometry(*cell);
                cell->input()->setText(QString(100, 'z'));
                require(mark(*cell)->state() == catalog::ValidationMark::State::Invalid,
                        "long unknown word is invalid");
                checkGeometry(*cell);
                cell->input()->clear();
                require(mark(*cell)->state() == catalog::ValidationMark::State::None,
                        "empty mnemonic word clears mark");
                checkGeometry(*cell);
            }
            window.hide();
        }
    }
}

auto input(account::view::Send &send, const QString &placeholder) -> catalog::Input * {
    for (auto *field : send.findChildren<catalog::Input *>()) {
        if (field->placeholderText() == placeholder) {
            return field;
        }
    }
    qFatal("validated input geometry: Send input missing");
}

auto wrapper(catalog::Input *input) -> catalog::ValidatedInput * {
    for (auto *parent = input->parentWidget(); parent != nullptr; parent = parent->parentWidget()) {
        if (auto *field = qobject_cast<catalog::ValidatedInput *>(parent); field != nullptr) {
            require(field->input() == input, "wrapper retains its underlying Input");
            return field;
        }
    }
    qFatal("validated input geometry: Send input does not use shared wrapper");
}

void sendFields() {
    PriceBackend backend;
    account::view::Send send(&backend);
    QPointer<catalog::Input> address = input(send, TR("send-placeholder-address"));
    QPointer<catalog::Input> btc = input(send, TR("send-placeholder-amount"));
    QPointer<catalog::Input> fee = send.findChild<catalog::Input *>(QStringLiteral("fee-value"));
    require(fee != nullptr, "fee-value remains on the underlying Input");
    require(btc->validator() != nullptr && fee->validator() != nullptr, "validators are preserved");
    address->setText(QStringLiteral("bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"));
    btc->setText(QStringLiteral("0.00000003"));
    fee->setText(QStringLiteral("1.234"));
    bool cursorSet = false;
    for (auto width : {1200, 1568, 1569, 1800, 1568, 1200}) {
        send.resize(width, 1000);
        send.show();
        settle(send);
        require(!address.isNull() && !btc.isNull() && !fee.isNull(),
                "breakpoint rebuild keeps underlying inputs alive");
        if (cursorSet) {
            require(address->cursorPosition() == 12, "breakpoint rebuild preserves address cursor");
        }
        for (auto *field : {address.data(), btc.data(), fee.data()}) {
            checkGeometry(*wrapper(field));
        }
        require(wrapper(btc)->width() == 150 && wrapper(fee)->width() == 100,
                "fixed-width Send wrappers do not add gaps around their inputs");
        require(mark(*wrapper(btc))->state() == catalog::ValidationMark::State::Valid,
                "BTC validation survives layout rebuild");
        require(mark(*wrapper(address))->state() == catalog::ValidationMark::State::Valid,
                "address validation survives layout rebuild");
        require(mark(*wrapper(fee))->state() == catalog::ValidationMark::State::Valid,
                "fee validation survives layout rebuild");
        send.activateWindow();
        QApplication::processEvents();
        address->setFocus();
        address->setCursorPosition(12);
        send.onProcess();
        require(address->hasFocus() && address->cursorPosition() == 12,
                "validation update preserves address focus and cursor");
        send.onSigningLocked(true);
        require(wrapper(fee)->isHidden() && !fee->isVisible(), "locked fee hides entire wrapper");
        send.onSigningLocked(false);
        require(!wrapper(fee)->isHidden() && fee->isVisible(), "unlock restores fee wrapper");
        settle(send);
        checkGeometry(*wrapper(fee));
        address->setFocus();
        address->setCursorPosition(12);
        cursorSet = true;
    }
    for (auto *field : {address.data(), btc.data(), fee.data()}) {
        field->setText(QString(300, 'z'));
        require(mark(*wrapper(field))->state() == catalog::ValidationMark::State::Invalid,
                "long invalid Send value is marked invalid");
        checkGeometry(*wrapper(field));
        field->clear();
        require(mark(*wrapper(field))->state() == catalog::ValidationMark::State::None,
                "empty Send value clears integrated mark");
        checkGeometry(*wrapper(field));
    }
    for (auto *toggle : send.findChildren<QCheckBox *>()) {
        if (toggle->property("outputId").isValid()) {
            toggle->setChecked(true);
            require(!btc->isEnabled(), "MAX disables BTC input");
            checkGeometry(*wrapper(btc));
            toggle->setChecked(false);
            require(btc->isEnabled(), "leaving MAX restores BTC input");
            break;
        }
    }
}

} // namespace

auto validatedInputGeometry() -> bool {
    fixedAndExpanding();
    wordGrids();
    sendFields();
    qInfo() << "ValidatedInput fixed/expanding, mnemonic grid and Send geometry checks passed";
    return true;
}

} // namespace smoke
