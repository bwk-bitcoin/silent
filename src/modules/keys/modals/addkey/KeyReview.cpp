#include "KeyReview.h"

#include "KeyChecks.h"
#include "KeyFields.h"

#include "catalog/Button.h"
#include "catalog/Table.h"
#include "catalog/Variants.h"
#include "catalog/containers/Modal.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/display/ShortValue.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/Field.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

KeyReview::KeyReview(KeysModule *module, const QString &title) : m_module(module) {
    init(title);
    doConnect();
    view();
}

void KeyReview::setKey(const QString &fingerprint, const QList<PathXpub> &xpubs,
                       const QString &alias) {
    m_fingerprint = fingerprint;
    m_xpubs = xpubs;
    m_fingerprint_label->setText(fingerprint);
    m_table->clearRows();
    for (const auto &entry : xpubs) {
        auto *path = new Label(entry.path, LabelRole::Mono);
        path->setTone(LabelTone::Secondary);
        m_table->addWidgetRow(entry.path, {path, new catalog::ShortValue(entry.xpub, false)});
    }
    m_alias->setText(alias);
    m_error->setVisible(false);
    onAliasEdited();
}

auto KeyReview::add(const QList<KeyRouteKind> &routes) -> bool {
    QString error = addKey(m_module, m_fingerprint, m_alias->text().trimmed(), routes, m_xpubs);
    if (!error.isEmpty()) {
        m_error->setText(error);
        m_error->setVisible(true);
        window()->adjustSize();
        return false;
    }
    return true;
}

auto KeyReview::header() const -> QWidget * {
    return m_header;
}

auto KeyReview::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void KeyReview::init(const QString &title) {
    m_fingerprint_label = new Label(LabelRole::Mono);
    m_fingerprint_label->setTone(LabelTone::Secondary);
    m_fingerprint_label->setSize(catalog::LabelSize::Sm);
    m_header = catalog::modalHeader(title, m_fingerprint_label);
    m_table = new catalog::Table;
    m_table->setHeaderVisible(false);
    m_table->addColumn({.width = metric::XPUB_PATH_COLUMN});
    m_table->addColumn({.grow = true});
    m_duplicate = duplicateNotice();
    m_alias = aliasInput();
    m_alias_field = new catalog::Field(TR("keys-alias"), m_alias);
    m_error = errorNotice();
    m_add = catalog::btnAddKey();
    m_actions = new catalog::ModalActions;
    m_actions->setBack(catalog::ModalActions::Back::Back);
    m_actions->addAction(m_add);
}

void KeyReview::doConnect() {
    connect(m_alias, &QLineEdit::textEdited, this, &KeyReview::onAliasEdited, qontrol::UNIQUE);
    connect(m_table, &catalog::Table::rebuilt, this, &KeyReview::onTableRebuilt, qontrol::UNIQUE);
    connect(m_add, &QPushButton::clicked, this, &KeyReview::addClicked, qontrol::UNIQUE);
    connect(m_actions, &catalog::ModalActions::backClicked, this, &KeyReview::backClicked,
            qontrol::UNIQUE);
}

void KeyReview::view() {
    setFitContent(true);
    setWidget((new qontrol::Column)
                  ->spacing(resolve(Spacing::M))
                  ->push(m_table)
                  ->push(m_duplicate)
                  ->push(m_alias_field)
                  ->push(m_error));
}

void KeyReview::onAliasEdited() {
    QString duplicate = registeredAlias(m_module, m_fingerprint);
    m_duplicate->setText(TR("keys-duplicate-body").arg(m_fingerprint, duplicate));
    m_duplicate->setVisible(!duplicate.isEmpty());
    QString reason = duplicate.isEmpty() ? aliasReason(m_module, m_alias->text().trimmed())
                                         : TR("keys-duplicate-reason");
    m_add->setEnabled(reason.isEmpty());
    m_actions->setReason(reason);
    window()->adjustSize();
}

void KeyReview::onTableRebuilt() {
    window()->adjustSize();
}

} // namespace keys::modal
