#include "AddKey.h"

#include "modules/keys/BwkQrEngine.h"
#include "modules/keys/KeysView.h"
#include "modules/keys/flow/CameraStep.h"
#include "modules/keys/flow/QrDisplayStep.h"
#include "qr/QrCameraEngine.h"
#include "qr/QrDisplayEngine.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Table.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/display/Label.h"
#include "catalog/display/ShortValue.h"
#include "catalog/display/WordGrid.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/Field.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/form/WordInputGrid.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMimeData>
#include <QRegularExpression>
#include <QUrl>

#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {

const char *const K_MODE = "addKeyMode";
const char *const K_HOT_KIND = "hotKind";

enum class Mode { Qr, File, Paste, Hot };

constexpr int KEY_FILE_VERSION = 1;
constexpr int MIN_PASSWORD_LENGTH = 8;
constexpr int WORDS_SHORT = 12;
constexpr int WORDS_MIDDLE = 15;
constexpr int WORDS_LONG = 24;

// [73c5da0a/84'/1'/0']tpubD...: the origin, then the key.
const QRegularExpression PASTED_KEY_RE(QStringLiteral(R"(^(\[[^\]]*\])\s*(\S+)$)"));
const QRegularExpression ORIGIN_RE(QStringLiteral(R"(^\[([0-9a-f]{8})((?:/\d+['h]?)+)\]$)"));
const QRegularExpression XPUB_RE(QStringLiteral(R"(^[xt]pub[1-9A-HJ-NP-Za-km-z]{100,}$)"));
const QRegularExpression FINGERPRINT_RE(QStringLiteral(R"(^[0-9a-f]{8}$)"));

auto networkFromFile(const QString &name) -> std::optional<Network> {
    if (name == QStringLiteral("bitcoin")) {
        return Network::Bitcoin;
    }
    if (name == QStringLiteral("testnet")) {
        return Network::Testnet;
    }
    if (name == QStringLiteral("signet")) {
        return Network::Signet;
    }
    if (name == QStringLiteral("regtest")) {
        return Network::Regtest;
    }
    return std::nullopt;
}

auto routeFromFile(const QString &name) -> std::optional<KeyRouteKind> {
    if (name == QStringLiteral("hot")) {
        return KeyRouteKind::Hot;
    }
    if (name == QStringLiteral("usb")) {
        return KeyRouteKind::Usb;
    }
    if (name == QStringLiteral("qr")) {
        return KeyRouteKind::Qr;
    }
    if (name == QStringLiteral("sdcard")) {
        return KeyRouteKind::SdCard;
    }
    return std::nullopt;
}

auto modeRow(Mode mode, IconFn icon, const QString &title, const QString &sub)
    -> catalog::SelectRow * {
    auto *row = new catalog::SelectRow;
    row->setIcon(icon);
    row->setTitle(title);
    row->setSub(sub);
    row->setProperty(K_MODE, static_cast<int>(mode));
    return row;
}

auto hotRow(int kind, IconFn icon, const QString &title, const QString &sub)
    -> catalog::SelectRow * {
    auto *row = new catalog::SelectRow;
    row->setIcon(icon);
    row->setTitle(title);
    row->setSub(sub);
    row->setProperty(K_HOT_KIND, kind);
    return row;
}

auto aliasInput() -> catalog::Input * {
    auto *input = new catalog::Input;
    input->setPlaceholderText(TR("keys-alias-name"));
    return input;
}

auto duplicateNotice() -> catalog::Notice * {
    auto *notice = new catalog::Notice(catalog::Notice::Tone::Error);
    notice->setTitle(TR("keys-duplicate-title"));
    notice->setVisible(false);
    return notice;
}

auto errorNotice() -> catalog::Notice * {
    auto *notice = new catalog::Notice(catalog::Notice::Tone::Error);
    notice->setVisible(false);
    return notice;
}

} // namespace

AddKey::AddKey(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
    showModes();
}

AddKey::~AddKey() {
    clearSecrets();
}

void AddKey::done(int result) {
    stopQr();
    clearSecrets();
    catalog::Modal::done(result);
}

void AddKey::init() {
    setAcceptDrops(true);
    initModes();
    initQr();
    initReview();
    initPaste();
    initHotChoose();
    initBackup();
    initHotPassword();
    initStatus();
}

void AddKey::doConnect() {
    connectModes();
    connectQr();
    connectReview();
    connectPaste();
    connectHotChoose();
    connectBackup();
    connectHotPassword();
    connectStatus();
}

void AddKey::view() {
    m_modes_body = viewModes();
    m_review_body = viewReview();
    m_paste_body = viewPaste();
    m_choose_body = viewHotChoose();
    m_backup_body = viewBackup();
    m_password_body = viewHotPassword();
    hideQr();
    hideReview();
    hidePaste();
    hideHotChoose();
    hideBackup();
    hideHotPassword();
    hideStatus();
}

// ===== Modes view =====

void AddKey::initModes() {
    m_mode_rows = {
        modeRow(Mode::Qr, icon::qr, TR("keys-add-mode-qr"), TR("keys-add-mode-qr-sub")),
        modeRow(Mode::File, icon::folder, TR("keys-add-mode-file"), TR("keys-add-mode-file-sub")),
        modeRow(Mode::Paste, icon::pencil, TR("keys-add-mode-paste"),
                TR("keys-add-mode-paste-sub")),
        modeRow(Mode::Hot, icon::key, TR("keys-add-mode-hot"), TR("keys-add-mode-hot-sub")),
    };
}

void AddKey::connectModes() {
    for (auto *row : m_mode_rows) {
        connect(row, &catalog::SelectRow::clicked, this, &AddKey::onModeClicked, qontrol::UNIQUE);
    }
}

auto AddKey::viewModes() -> QWidget * {
    auto *column = (new qontrol::Column)->spacing(resolve(Spacing::S));
    for (auto *row : m_mode_rows) {
        column->push(row);
    }
    return column;
}

void AddKey::showModes() {
    m_view = View::Modes;
    stopQr();
    setWindowTitle(TR("keys-add-title"));
    setHeader(nullptr);
    setBody(m_modes_body);
    setFooter(nullptr);
    setWidth(ModalWidth::L);
}

// ===== QR code and camera views =====

void AddKey::initQr() {
    m_qr_engine = new BwkQrEngine(m_module, this);
    m_qr = new flow::QrDisplayStep;
    m_qr->setHint(TR("keys-add-qr-hint"));
    m_qr->setTunable(false);
    m_qr->setSize(metric::QR_REQUEST_SIZE);
    m_qr->actions()->setBack(catalog::ModalActions::Back::Back);
    m_camera = new flow::CameraStep;
    m_camera->setHint(TR("keys-add-scan-hint"));
}

void AddKey::connectQr() {
    connect(m_qr, &flow::QrDisplayStep::nextClicked, this, &AddKey::onNextClicked, qontrol::UNIQUE);
    connect(m_qr->actions(), &catalog::ModalActions::backClicked, this, &AddKey::onBackClicked,
            qontrol::UNIQUE);
    connect(m_camera, &flow::CameraStep::backClicked, this, &AddKey::onBackClicked,
            qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::request, m_qr->engine(), &qr::QrDisplayEngine::request,
            qontrol::UNIQUE);
    connect(m_camera->engine(), &qr::QrCameraEngine::response, m_qr_engine, &qr::QrEngine::response,
            qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::xpub, this, &AddKey::onQrXpub, qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::error, this, &AddKey::onQrError, qontrol::UNIQUE);
}

void AddKey::showQr() {
    showQrDisplay();
    m_qr_engine->getXpub(KeysModule::defaultXpubPath());
}

void AddKey::showQrDisplay() {
    m_view = View::QrDisplay;
    setWindowTitle(TR("keys-add-qr-title"));
    setHeader(nullptr);
    setBody(m_qr);
    setFooter(m_qr->actions());
    setWidth(ModalWidth::L);
}

void AddKey::showCamera() {
    m_view = View::Camera;
    setWindowTitle(TR("keys-add-scan-title"));
    setHeader(nullptr);
    setBody(m_camera);
    setFooter(m_camera->actions());
    setWidth(ModalWidth::XXL);
}

void AddKey::stopQr() {
    m_qr_engine->stop();
    m_qr->stop();
    m_camera->stop();
}

void AddKey::hideQr() {
    keep(m_qr);
    keep(m_qr->actions());
    keep(m_camera);
    keep(m_camera->actions());
}

// ===== Review view =====

void AddKey::initReview() {
    m_drop = new catalog::Card(catalog::Card::Role::Inset);
    m_drop->setPad(catalog::Card::Pad::M);
    auto *folder = new QLabel;
    folder->setPixmap(
        icon::folder(IconColor::Muted).pixmap(metric::DROP_ICON_SIZE, metric::DROP_ICON_SIZE));
    auto *dropCaption = new Label(TR("keys-file-drop"), LabelRole::Caption);
    dropCaption->setTone(LabelTone::Muted);
    m_drop->setContent((new qontrol::Column)
                           ->margins(0, resolve(Spacing::L), 0, resolve(Spacing::L))
                           ->spacing(resolve(Spacing::S))
                           ->pushAligned(folder, Qt::AlignHCenter)
                           ->pushAligned(dropCaption, Qt::AlignHCenter));
    m_read_error = errorNotice();
    m_read_error->setTitle(TR("keys-file-read-title"));
    m_read_error->setText(TR("keys-file-read-body"));
    m_file_error = errorNotice();
    m_file_error->setTitle(TR("keys-file-error-title"));
    m_review_table = new catalog::Table;
    m_review_table->setHeaderVisible(false);
    m_review_table->addColumn({.width = metric::XPUB_PATH_COLUMN});
    m_review_table->addColumn({.grow = true});
    m_review_duplicate = duplicateNotice();
    m_review_alias = aliasInput();
    m_review_alias_field = new catalog::Field(TR("keys-alias"), m_review_alias);
    m_review_error = errorNotice();
    m_open_file = catalog::btnOpenFile();
    m_add_reviewed = catalog::btnAddKey();
    m_review_actions = new catalog::ModalActions;
    m_review_actions->setBack(catalog::ModalActions::Back::Back);
    m_review_actions->addAction(m_open_file);
    m_review_actions->addAction(m_add_reviewed);
}

void AddKey::connectReview() {
    connect(m_review_alias, &QLineEdit::textEdited, this, &AddKey::onReviewChanged,
            qontrol::UNIQUE);
    connect(m_review_table, &catalog::Table::rebuilt, this, &AddKey::adjustSize, qontrol::UNIQUE);
    connect(m_open_file, &QPushButton::clicked, this, &AddKey::onOpenFileClicked, qontrol::UNIQUE);
    connect(m_add_reviewed, &QPushButton::clicked, this, &AddKey::onNextClicked, qontrol::UNIQUE);
    connect(m_review_actions, &catalog::ModalActions::backClicked, this, &AddKey::onBackClicked,
            qontrol::UNIQUE);
}

auto AddKey::viewReview() -> catalog::ScrollArea * {
    m_file_area = (new qontrol::Column)
                      ->spacing(resolve(Spacing::S))
                      ->push(m_drop)
                      ->push(m_read_error)
                      ->push(m_file_error);
    auto *column = (new qontrol::Column)
                       ->spacing(resolve(Spacing::M))
                       ->push(m_file_area)
                       ->push(m_review_table)
                       ->push(m_review_duplicate)
                       ->push(m_review_alias_field)
                       ->push(m_review_error);
    auto *scroll = new catalog::ScrollArea;
    scroll->setFitContent(true);
    scroll->setWidget(column);
    return scroll;
}

void AddKey::showFile() {
    m_review_xpubs.clear();
    m_review_fingerprint.clear();
    m_read_error->setVisible(false);
    m_file_error->setVisible(false);
    m_review_alias->clear();
    showReview(Source::File);
}

void AddKey::showReview(Source source) {
    m_view = View::Review;
    m_source = source;
    m_review_table->clearRows();
    for (const auto &entry : m_review_xpubs) {
        auto *path = new Label(entry.path, LabelRole::Mono);
        path->setTone(LabelTone::Secondary);
        m_review_table->addWidgetRow(entry.path,
                                     {path, new catalog::ShortValue(entry.xpub, false)});
    }
    m_review_error->setVisible(false);
    onReviewChanged();

    QString title = source == Source::Qr ? TR("keys-add-review-title") : TR("keys-add-file-title");
    setWindowTitle(title);
    // The loaded key's fingerprint sits on the right of the title.
    auto *old = m_header;
    m_header = nullptr;
    if (!m_review_fingerprint.isEmpty()) {
        auto *fingerprint = new Label(m_review_fingerprint, LabelRole::Mono);
        fingerprint->setTone(LabelTone::Secondary);
        fingerprint->setSize(catalog::LabelSize::Sm);
        m_header = catalog::modalHeader(title, fingerprint);
    }
    setHeader(m_header);
    delete old;
    setBody(m_review_body);
    setFooter(m_review_actions);
    setWidth(ModalWidth::L);
}

void AddKey::hideReview() {
    keep(m_review_body);
    keep(m_review_actions);
}

void AddKey::addReviewed() {
    addKey(m_review_fingerprint, m_review_alias->text().trimmed(), m_review_routes, m_review_xpubs,
           m_review_error);
}

void AddKey::dragEnterEvent(QDragEnterEvent *event) {
    bool waitingForFile = m_review_body->isVisibleTo(this) && m_source == Source::File &&
                          m_review_fingerprint.isEmpty();
    if (waitingForFile && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void AddKey::dropEvent(QDropEvent *event) {
    QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile()) {
        event->acceptProposedAction();
        loadFile(urls.first().toLocalFile());
    }
}

// A key file as Silent writes it: version, network, fingerprint, alias, xpubs as
// path and xpub pairs, routes. Device exports in their own formats are not read.
void AddKey::loadFile(const QString &path) {
    m_read_error->setVisible(false);
    m_file_error->setVisible(false);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_read_error->setVisible(true);
        adjustSize();
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        showFileError(TR("keys-file-invalid"));
        return;
    }
    QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("version")).toInt() > KEY_FILE_VERSION) {
        showFileError(TR("keys-file-newer"));
        return;
    }
    auto network = networkFromFile(obj.value(QStringLiteral("network")).toString());
    if (!network.has_value()) {
        showFileError(TR("keys-file-invalid"));
        return;
    }
    if (network.value() != KeysModule::network()) {
        showFileError(TR("keys-file-other-network")
                          .arg(networkLabel(network.value()), networkLabel(KeysModule::network())));
        return;
    }
    QString fingerprint = obj.value(QStringLiteral("fingerprint")).toString().trimmed().toLower();
    if (fingerprint.isEmpty()) {
        showFileError(TR("keys-file-no-fingerprint"));
        return;
    }
    if (!FINGERPRINT_RE.match(fingerprint).hasMatch()) {
        showFileError(TR("keys-file-invalid"));
        return;
    }
    QList<KeyRouteKind> routes;
    for (const auto &value : obj.value(QStringLiteral("routes")).toArray()) {
        auto route = routeFromFile(value.toString());
        if (!route.has_value()) {
            showFileError(TR("keys-file-route").arg(value.toString()));
            return;
        }
        routes.append(route.value());
    }
    QList<PathXpub> xpubs;
    for (const auto &value : obj.value(QStringLiteral("xpubs")).toArray()) {
        QJsonObject entry = value.toObject();
        xpubs.append({.path = xpubPath(entry.value(QStringLiteral("path")).toString()),
                      .xpub = entry.value(QStringLiteral("xpub")).toString()});
    }

    m_review_fingerprint = fingerprint;
    m_review_xpubs = xpubs;
    // A file names the routes the key had; an export without any came off an SD card.
    m_review_routes = routes.isEmpty() ? QList<KeyRouteKind>{KeyRouteKind::SdCard} : routes;
    m_review_alias->setText(obj.value(QStringLiteral("alias")).toString());
    showReview(Source::File);
}

void AddKey::showFileError(const QString &error) {
    m_file_error->setText(error);
    m_file_error->setVisible(true);
    adjustSize();
}

// ===== Paste view =====

void AddKey::initPaste() {
    m_paste = new catalog::ValidatedInput(catalog::InputRole::Mono);
    m_paste->input()->setPlaceholderText(TR("keys-paste-placeholder")
                                             .arg(KeysModule::network() == Network::Bitcoin
                                                      ? QStringLiteral("xpub")
                                                      : QStringLiteral("tpub")));
    m_paste_note =
        new catalog::FieldNote(TR("keys-paste-invalid"), catalog::FieldNote::Tone::Error);
    m_paste_note->setVisible(false);
    m_paste_alias = aliasInput();
    m_paste_duplicate = duplicateNotice();
    m_paste_error = errorNotice();
    m_add_pasted = catalog::btnAddKey();
    m_paste_actions = new catalog::ModalActions;
    m_paste_actions->setBack(catalog::ModalActions::Back::Back);
    m_paste_actions->addAction(m_add_pasted);
}

void AddKey::connectPaste() {
    connect(m_paste, &catalog::ValidatedInput::textChanged, this, &AddKey::onPasteChanged,
            qontrol::UNIQUE);
    connect(m_paste_alias, &QLineEdit::textEdited, this, &AddKey::onPasteChanged, qontrol::UNIQUE);
    connect(m_add_pasted, &QPushButton::clicked, this, &AddKey::onNextClicked, qontrol::UNIQUE);
    connect(m_paste_actions, &catalog::ModalActions::backClicked, this, &AddKey::onBackClicked,
            qontrol::UNIQUE);
}

auto AddKey::viewPaste() -> QWidget * {
    auto *fields = new catalog::FieldGroup;
    fields->addField(TR("keys-xpub"), m_paste)
        ->addNote(m_paste_note)
        ->addField(TR("keys-alias"), m_paste_alias);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::S))
        ->push(fields)
        ->push(m_paste_duplicate)
        ->push(m_paste_error);
}

void AddKey::showPaste() {
    m_view = View::Paste;
    setWindowTitle(TR("keys-add-paste-title"));
    setHeader(nullptr);
    setBody(m_paste_body);
    setFooter(m_paste_actions);
    setWidth(ModalWidth::L);
}

void AddKey::hidePaste() {
    keep(m_paste_body);
    keep(m_paste_actions);
}

auto AddKey::pastedKey() const -> std::optional<PastedKey> {
    auto match = PASTED_KEY_RE.match(m_paste->input()->text().trimmed());
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    auto origin = ORIGIN_RE.match(match.captured(1));
    QString xpub = match.captured(2);
    if (!origin.hasMatch() || !XPUB_RE.match(xpub).hasMatch()) {
        return std::nullopt;
    }
    return PastedKey{.fingerprint = origin.captured(1),
                     .xpub = {.path = QStringLiteral("m") + origin.captured(2), .xpub = xpub}};
}

void AddKey::addPasted() {
    auto key = pastedKey();
    if (!key.has_value()) {
        return;
    }
    // A pasted xpub comes from an airgapped device, which signs over its SD card.
    addKey(key->fingerprint, m_paste_alias->text().trimmed(), {KeyRouteKind::SdCard}, {key->xpub},
           m_paste_error);
}

// ===== Hot key choice view =====

void AddKey::initHotChoose() {
    m_generate12 = hotRow(static_cast<int>(HotKind::Generate12), icon::refresh,
                          TR("keys-hot-generate-12"), TR("keys-hot-generate-12-sub"));
    m_generate24 = hotRow(static_cast<int>(HotKind::Generate24), icon::refresh,
                          TR("keys-hot-generate-24"), TR("keys-hot-generate-24-sub"));
    m_import = hotRow(static_cast<int>(HotKind::Import), icon::pencil, TR("keys-hot-import-words"),
                      TR("keys-hot-import-sub"));
    m_word_count = new catalog::ComboBox;
    for (int count : {WORDS_SHORT, WORDS_MIDDLE, WORDS_LONG}) {
        m_word_count->addItem(TR("keys-words-count").arg(count), count);
    }
    m_word_count->setWidth(Size::S);
    m_import->setTrailing(m_word_count);
    m_words_input = new catalog::WordInputGrid;
    m_words_input->setWordCount(WORDS_SHORT);
    m_words_input->setWordCheck(
        [](const QString &word) { return ::bip39_word_is_valid(word.toStdString()); });
    m_choose_continue = catalog::btnContinue();
    m_choose_actions = new catalog::ModalActions;
    m_choose_actions->setBack(catalog::ModalActions::Back::Back);
    m_choose_actions->addAction(m_choose_continue);
}

void AddKey::connectHotChoose() {
    for (auto *row : {m_generate12, m_generate24, m_import}) {
        connect(row, &catalog::SelectRow::clicked, this, &AddKey::onHotKindClicked,
                qontrol::UNIQUE);
    }
    connect(m_word_count, &QComboBox::activated, this, &AddKey::onWordCountPicked, qontrol::UNIQUE);
    connect(m_words_input, &catalog::WordInputGrid::wordsChanged, this, &AddKey::onHotChooseChanged,
            qontrol::UNIQUE);
    connect(m_choose_continue, &QPushButton::clicked, this, &AddKey::onNextClicked,
            qontrol::UNIQUE);
    connect(m_choose_actions, &catalog::ModalActions::backClicked, this, &AddKey::onBackClicked,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::mnemonicGenerated, this, &AddKey::onMnemonicGenerated,
            qontrol::UNIQUE);
}

auto AddKey::viewHotChoose() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push((new qontrol::Column)
                   ->spacing(resolve(Spacing::S))
                   ->push(m_generate12)
                   ->push(m_generate24)
                   ->push(m_import))
        ->push(m_words_input);
}

void AddKey::showHotChoose() {
    m_view = View::HotChoose;
    onHotChooseChanged();
    setWindowTitle(TR("keys-add-hot-title"));
    setHeader(nullptr);
    setBody(m_choose_body);
    setFooter(m_choose_actions);
    setWidth(ModalWidth::L);
}

void AddKey::hideHotChoose() {
    keep(m_choose_body);
    keep(m_choose_actions);
}

auto AddKey::typedMnemonic() const -> QString {
    return m_words_input->words().join(QLatin1Char(' ')).trimmed().toLower();
}

void AddKey::continueHot() {
    switch (m_hot_kind) {
    case HotKind::Import:
        m_mnemonic = typedMnemonic();
        showHotPassword();
        return;
    case HotKind::Generate12:
    case HotKind::Generate24:
        m_choose_continue->setEnabled(false);
        m_generate_req = m_module->nextReqId();
        m_module->generateMnemonic(m_generate_req,
                                   m_hot_kind == HotKind::Generate12 ? WORDS_SHORT : WORDS_LONG);
        return;
    }
}

// ===== Backup view =====

void AddKey::initBackup() {
    m_backup_words = new catalog::WordGrid;
    m_backup_check = new catalog::Checkbox(TR("keys-backup-wrote"));
    m_backup_continue = catalog::btnContinue();
    m_backup_actions = new catalog::ModalActions;
    m_backup_actions->setBack(catalog::ModalActions::Back::Back);
    m_backup_actions->addAction(m_backup_continue);
}

void AddKey::connectBackup() {
    connect(m_backup_check, &QCheckBox::toggled, this, &AddKey::onBackupChanged, qontrol::UNIQUE);
    connect(m_backup_continue, &QPushButton::clicked, this, &AddKey::onNextClicked,
            qontrol::UNIQUE);
    connect(m_backup_actions, &catalog::ModalActions::backClicked, this, &AddKey::onBackClicked,
            qontrol::UNIQUE);
}

auto AddKey::viewBackup() -> QWidget * {
    auto *warning = new catalog::Notice(catalog::Notice::Tone::Warning);
    warning->setTitle(TR("keys-backup-title"));
    warning->setText(TR("keys-backup-body"));
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(warning)
        ->push(m_backup_words)
        ->push(m_backup_check);
}

void AddKey::showBackup() {
    m_view = View::Backup;
    setWindowTitle(TR("keys-add-backup-title"));
    setHeader(nullptr);
    setBody(m_backup_body);
    setFooter(m_backup_actions);
    setWidth(ModalWidth::L);
}

void AddKey::hideBackup() {
    keep(m_backup_body);
    keep(m_backup_actions);
}

// ===== Hot key password view =====

void AddKey::initHotPassword() {
    m_hot_alias = aliasInput();
    m_password = new catalog::Input;
    m_password->setEchoMode(QLineEdit::Password);
    m_password_short =
        new catalog::FieldNote(TR("keys-password-short"), catalog::FieldNote::Tone::Error);
    m_confirm = new catalog::Input;
    m_confirm->setEchoMode(QLineEdit::Password);
    m_password_mismatch =
        new catalog::FieldNote(TR("keys-password-no-match"), catalog::FieldNote::Tone::Error);
    m_seal = catalog::btnSealKey();
    m_password_actions = new catalog::ModalActions;
    m_password_actions->setBack(catalog::ModalActions::Back::Back);
    m_password_actions->addAction(m_seal);
}

void AddKey::connectHotPassword() {
    for (auto *input : {m_hot_alias, m_password, m_confirm}) {
        connect(input, &QLineEdit::textEdited, this, &AddKey::onHotPasswordChanged,
                qontrol::UNIQUE);
    }
    connect(m_seal, &QPushButton::clicked, this, &AddKey::onNextClicked, qontrol::UNIQUE);
    connect(m_password_actions, &catalog::ModalActions::backClicked, this, &AddKey::onBackClicked,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::hotKeyImported, this, &AddKey::onHotKeyImported,
            qontrol::UNIQUE);
}

auto AddKey::viewHotPassword() -> QWidget * {
    auto *lead = new Label(TR("keys-password-lead"), LabelRole::Body);
    lead->setTone(LabelTone::Secondary);
    lead->setWordWrap(true);
    m_password_fields = new catalog::FieldGroup;
    m_password_fields->addField(TR("keys-alias"), m_hot_alias)
        ->addField(TR("keys-password-field"), m_password)
        ->addNote(m_password_short)
        ->addField(TR("keys-password-confirm-field"), m_confirm)
        ->addNote(m_password_mismatch);
    return (new qontrol::Column)->spacing(resolve(Spacing::S))->push(lead)->push(m_password_fields);
}

void AddKey::showHotPassword() {
    m_view = View::HotPassword;
    onHotPasswordChanged();
    setWindowTitle(TR("keys-add-password-title"));
    setHeader(nullptr);
    setBody(m_password_body);
    setFooter(m_password_actions);
    setWidth(ModalWidth::L);
}

void AddKey::hideHotPassword() {
    keep(m_password_body);
    keep(m_password_actions);
}

void AddKey::sealKey() {
    m_status->setSpinner();
    m_status->setTitle(TR("keys-sealing-title"));
    m_status->setSubtitle(TR("keys-sealing-sub"));
    showSealing();
    m_seal_req = m_module->nextReqId();
    m_module->importHotKey(m_seal_req, m_mnemonic, m_hot_alias->text().trimmed(),
                           m_password->text());
}

// ===== Status view =====

void AddKey::initStatus() {
    m_status = new catalog::ModalStatus;
    m_status->setPadding(resolve(Spacing::M), resolve(Spacing::M));
    m_done = catalog::btnDone();
    m_try_again = catalog::btnTryAgain();
    m_status_actions = new catalog::ModalActions;
    m_status_actions->addAction(m_done);
    m_status_actions->addAction(m_try_again);
}

void AddKey::connectStatus() {
    connect(m_done, &QPushButton::clicked, this, &AddKey::onNextClicked, qontrol::UNIQUE);
    connect(m_try_again, &QPushButton::clicked, this, &AddKey::onFailureRetry, qontrol::UNIQUE);
}

void AddKey::showSealing() {
    m_view = View::Sealing;
    setWindowTitle(TR("keys-add-sealing-title"));
    setHeader(nullptr);
    setBody(m_status);
    setFooter(nullptr);
    setWidth(ModalWidth::L);
}

void AddKey::showResult() {
    m_view = View::Result;
    setWindowTitle(TR("keys-add-title"));
    setHeader(nullptr);
    setBody(m_status);
    setFooter(m_status_actions);
    setWidth(ModalWidth::L);
}

void AddKey::hideStatus() {
    keep(m_status);
    keep(m_status_actions);
}

void AddKey::showFailure(Failure failure, const QString &title, const QString &message) {
    m_failure = failure;
    m_status->setIcon(icon::circleX, IconColor::Error);
    m_status->setTitle(title);
    m_status->setSubtitle(message);
    m_done->setVisible(false);
    m_try_again->setVisible(true);
    showResult();
}

// ===== Shared =====

void AddKey::clearSecrets() {
    // A QString cannot be zeroized: fill() and clear() are best effort. Rust owns the
    // real wiping of the mnemonic and the password it was handed.
    m_mnemonic.fill(QChar());
    m_mnemonic.clear();
    m_backup_words->setWords({});
    m_password->clear();
    m_confirm->clear();
}

auto AddKey::aliasReason(const QString &alias) const -> QString {
    if (alias.isEmpty()) {
        return TR("keys-alias-empty");
    }
    for (const auto &info : m_module->keys()) {
        if (KeysModule::toQt(info.alias) == alias) {
            return TR("keys-alias-taken-reason");
        }
    }
    return QString();
}

auto AddKey::registeredAlias(const QString &fingerprint) const -> QString {
    if (fingerprint.isEmpty()) {
        return QString();
    }
    for (const auto &info : m_module->keys()) {
        if (QString::compare(KeysModule::toQt(info.fingerprint), fingerprint,
                             Qt::CaseInsensitive) == 0) {
            return KeysModule::toQt(info.alias);
        }
    }
    return QString();
}

void AddKey::addKey(const QString &fingerprint, const QString &alias,
                    const QList<KeyRouteKind> &routes, const QList<PathXpub> &xpubs,
                    catalog::Notice *error) {
    auto result = m_module->addKey(fingerprint, alias, routes, xpubs);
    if (!result.is_ok) {
        error->setText(TR("keys-add-failed").arg(KeysModule::toQt(result.error)));
        error->setVisible(true);
        adjustSize();
        return;
    }
    accept();
}

// ===== Slots =====

void AddKey::onBackClicked() {
    switch (m_view) {
    case View::QrDisplay:
    case View::Paste:
    case View::HotChoose:
        showModes();
        return;
    case View::Camera:
        m_camera->stop();
        showQrDisplay();
        m_qr->restart();
        return;
    case View::Review:
        m_review_fingerprint.clear();
        if (m_source == Source::Qr) {
            showQr();
            return;
        }
        showModes();
        return;
    case View::Backup:
        showHotChoose();
        return;
    case View::HotPassword:
        if (m_hot_kind == HotKind::Import) {
            showHotChoose();
            return;
        }
        showBackup();
        return;
    case View::Modes:
    case View::Sealing:
    case View::Result:
        return;
    }
}

void AddKey::onNextClicked() {
    switch (m_view) {
    case View::QrDisplay:
        m_qr->stop();
        showCamera();
        m_camera->start();
        return;
    case View::Review:
        addReviewed();
        return;
    case View::Paste:
        addPasted();
        return;
    case View::HotChoose:
        continueHot();
        return;
    case View::Backup:
        showHotPassword();
        return;
    case View::HotPassword:
        sealKey();
        return;
    case View::Result:
        accept();
        return;
    case View::Modes:
    case View::Camera:
    case View::Sealing:
        return;
    }
}

void AddKey::onModeClicked() {
    switch (static_cast<Mode>(sender()->property(K_MODE).toInt())) {
    case Mode::Qr:
        showQr();
        return;
    case Mode::File:
        showFile();
        return;
    case Mode::Paste:
        m_paste->input()->clear();
        m_paste_alias->clear();
        m_paste_error->setVisible(false);
        onPasteChanged();
        showPaste();
        return;
    case Mode::Hot:
        showHotChoose();
        return;
    }
}

void AddKey::onQrXpub(const QString &fingerprint, const QString &path, const QString &xpub) {
    stopQr();
    m_review_fingerprint = fingerprint;
    m_review_xpubs = {{.path = path, .xpub = xpub}};
    m_review_routes = {KeyRouteKind::Qr};
    m_review_alias->clear();
    showReview(Source::Qr);
}

void AddKey::onQrError(qr::QrError kind, const QString &message) {
    if (kind != qr::QrError::RequestFailed) {
        // A refused answer: the scan goes on.
        m_camera->showScanError(kind, message);
        adjustSize();
        return;
    }
    stopQr();
    showFailure(Failure::Qr, TR("keys-qr-failed-title"), message);
}

void AddKey::onReviewChanged() {
    bool loaded = !m_review_fingerprint.isEmpty();
    m_file_area->setVisible(!loaded);
    m_review_table->setVisible(loaded);
    m_review_alias_field->setVisible(loaded);
    m_open_file->setVisible(!loaded);
    m_add_reviewed->setVisible(loaded);

    QString duplicate = registeredAlias(m_review_fingerprint);
    m_review_duplicate->setText(TR("keys-duplicate-body").arg(m_review_fingerprint, duplicate));
    m_review_duplicate->setVisible(loaded && !duplicate.isEmpty());
    QString reason;
    if (loaded) {
        reason = duplicate.isEmpty() ? aliasReason(m_review_alias->text().trimmed())
                                     : TR("keys-duplicate-reason");
    }
    m_add_reviewed->setEnabled(loaded && reason.isEmpty());
    m_review_actions->setReason(reason);
    adjustSize();
}

void AddKey::onOpenFileClicked() {
    QString path =
        QFileDialog::getOpenFileName(this, TR("keys-pick-file"), QString(), TR("keys-file-filter"));
    if (!path.isEmpty()) {
        loadFile(path);
    }
}

void AddKey::onPasteChanged() {
    QString text = m_paste->input()->text().trimmed();
    auto key = pastedKey();
    if (text.isEmpty()) {
        m_paste->setState(catalog::ValidationMark::State::None);
    } else {
        m_paste->setState(key.has_value() ? catalog::ValidationMark::State::Valid
                                          : catalog::ValidationMark::State::Invalid);
    }
    m_paste_note->setVisible(!text.isEmpty() && !key.has_value());
    QString duplicate = key.has_value() ? registeredAlias(key->fingerprint) : QString();
    m_paste_duplicate->setText(
        TR("keys-duplicate-body").arg(key.has_value() ? key->fingerprint : QString(), duplicate));
    m_paste_duplicate->setVisible(!duplicate.isEmpty());
    QString reason;
    if (key.has_value()) {
        reason = duplicate.isEmpty() ? aliasReason(m_paste_alias->text().trimmed())
                                     : TR("keys-duplicate-reason");
    }
    m_add_pasted->setEnabled(key.has_value() && reason.isEmpty());
    m_paste_actions->setReason(reason);
    adjustSize();
}

void AddKey::onHotKindClicked() {
    m_hot_kind = static_cast<HotKind>(sender()->property(K_HOT_KIND).toInt());
    showHotChoose();
}

void AddKey::onWordCountPicked() {
    m_hot_kind = HotKind::Import;
    m_words_input->setWordCount(m_word_count->currentData().toInt());
    onHotChooseChanged();
}

void AddKey::onHotChooseChanged() {
    m_generate12->setSelected(m_hot_kind == HotKind::Generate12);
    m_generate24->setSelected(m_hot_kind == HotKind::Generate24);
    m_import->setSelected(m_hot_kind == HotKind::Import);
    bool importing = m_hot_kind == HotKind::Import;
    m_words_input->setVisible(importing);

    QString reason;
    if (importing) {
        int missing = 0;
        int unknown = 0;
        for (const auto &word : m_words_input->words()) {
            if (word.isEmpty()) {
                ++missing;
            } else if (!::bip39_word_is_valid(word.toStdString())) {
                ++unknown;
            }
        }
        if (unknown > 0) {
            reason = unknown == 1 ? TR("keys-words-unknown-one")
                                  : TR("keys-words-unknown-many").arg(unknown);
        } else if (missing > 0) {
            reason = missing == 1 ? TR("keys-words-missing-one")
                                  : TR("keys-words-missing-many").arg(missing);
        } else if (!::mnemonic_is_valid(typedMnemonic().toStdString())) {
            reason = TR("keys-words-checksum");
        }
    }
    m_choose_continue->setEnabled(reason.isEmpty() && m_generate_req == 0);
    m_choose_actions->setReason(reason);
    adjustSize();
}

void AddKey::onMnemonicGenerated(ReqId req_id, const MnemonicResult &result) {
    if (m_generate_req == 0 || req_id != m_generate_req) {
        return;
    }
    m_generate_req = 0;
    if (!result.is_ok) {
        onHotChooseChanged();
        showFailure(Failure::Seal, TR("keys-generate-failed-title"),
                    KeysModule::toQt(result.error));
        return;
    }
    m_mnemonic = KeysModule::toQt(result.mnemonic);
    m_backup_words->setWords(m_mnemonic.split(QLatin1Char(' '), Qt::SkipEmptyParts));
    m_backup_check->setChecked(false);
    onBackupChanged();
    showBackup();
}

void AddKey::onBackupChanged() {
    bool wrote = m_backup_check->isChecked();
    m_backup_continue->setEnabled(wrote);
    m_backup_actions->setReason(wrote ? QString() : TR("keys-backup-reason"));
}

void AddKey::onHotPasswordChanged() {
    QString password = m_password->text();
    bool tooShort = !password.isEmpty() && password.size() < MIN_PASSWORD_LENGTH;
    bool mismatch = !m_confirm->text().isEmpty() && m_confirm->text() != password;
    m_password_short->setVisible(tooShort);
    m_password_mismatch->setVisible(mismatch);

    QString reason = aliasReason(m_hot_alias->text().trimmed());
    if (reason.isEmpty() && password.isEmpty()) {
        reason = TR("keys-password-choose");
    } else if (reason.isEmpty() && tooShort) {
        reason = TR("keys-password-short");
    } else if (reason.isEmpty() && m_confirm->text() != password) {
        reason = TR("keys-password-must-match");
    }
    m_seal->setEnabled(reason.isEmpty());
    m_password_actions->setReason(reason);
    adjustSize();
}

void AddKey::onHotKeyImported(ReqId req_id, const KeyInfoResult &result) {
    if (m_seal_req == 0 || req_id != m_seal_req) {
        return;
    }
    m_seal_req = 0;
    if (!result.is_ok) {
        showFailure(Failure::Seal, TR("keys-seal-failed-title"), KeysModule::toQt(result.error));
        return;
    }
    clearSecrets();
    m_status->setIcon(icon::circleCheck, IconColor::Success);
    m_status->setTitle(TR("keys-added-title"));
    m_status->setSubtitle(
        TR("keys-added-sub")
            .arg(KeysModule::toQt(result.key.alias), KeysModule::toQt(result.key.fingerprint)));
    m_done->setVisible(true);
    m_try_again->setVisible(false);
    showResult();
}

void AddKey::onFailureRetry() {
    switch (m_failure) {
    case Failure::Qr:
        showQr();
        return;
    case Failure::Seal:
        if (m_mnemonic.isEmpty()) {
            showHotChoose();
            return;
        }
        sealKey();
        return;
    }
}

} // namespace keys::modal
