#include "FileFlow.h"

#include "KeyFields.h"
#include "KeyReview.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysView.h"
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
#include <optional>

#include <Qontrol>
#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {

constexpr int KEY_FILE_VERSION = 1;

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

} // namespace

FileFlow::FileFlow(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
}

void FileFlow::start() {
    showDrop();
}

void FileFlow::init() {
    setAcceptDrops(true);
    initDrop();
    initReview();
}

void FileFlow::doConnect() {
    connectDrop();
    connectReview();
}

void FileFlow::view() {
    m_drop_body = viewDrop();
    hideReview();
}

// ===== Drop view =====

void FileFlow::initDrop() {
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
    m_open_file = catalog::btnOpenFile();
    m_drop_actions = new catalog::ModalActions;
    m_drop_actions->setBack(catalog::ModalActions::Back::Back);
    m_drop_actions->addAction(m_open_file);
}

void FileFlow::connectDrop() {
    connect(m_open_file, &QPushButton::clicked, this, &FileFlow::onOpenFileClicked,
            qontrol::UNIQUE);
    connect(m_drop_actions, &catalog::ModalActions::backClicked, this, &FileFlow::onBackClicked,
            qontrol::UNIQUE);
}

auto FileFlow::viewDrop() -> catalog::ScrollArea * {
    auto *scroll = new catalog::ScrollArea;
    scroll->setFitContent(true);
    scroll->setWidget((new qontrol::Column)
                          ->spacing(resolve(Spacing::S))
                          ->push(m_drop)
                          ->push(m_read_error)
                          ->push(m_file_error));
    return scroll;
}

void FileFlow::showDrop() {
    m_view = View::Drop;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_drop_body);
    viewSlots()->setFooter(m_drop_actions);
    emit viewChanged(TR("keys-add-file-title"));
}

void FileFlow::dragEnterEvent(QDragEnterEvent *event) {
    if (m_view == View::Drop && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void FileFlow::dropEvent(QDropEvent *event) {
    QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile()) {
        event->acceptProposedAction();
        loadFile(urls.first().toLocalFile());
    }
}

// A key file as Silent writes it: version, network, fingerprint, alias, xpubs as
// path and xpub pairs, routes. Device exports in their own formats are not read.
void FileFlow::loadFile(const QString &path) {
    m_read_error->setVisible(false);
    m_file_error->setVisible(false);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_read_error->setVisible(true);
        window()->adjustSize();
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

    // A file names the routes the key had; an export without any came off an SD card.
    m_routes = routes.isEmpty() ? QList<KeyRouteKind>{KeyRouteKind::SdCard} : routes;
    m_review->setKey(fingerprint, xpubs, obj.value(QStringLiteral("alias")).toString());
    showReview();
}

void FileFlow::showFileError(const QString &error) {
    m_file_error->setText(error);
    m_file_error->setVisible(true);
    window()->adjustSize();
}

// ===== Review view =====

void FileFlow::initReview() {
    m_review = new KeyReview(m_module, TR("keys-add-file-title"));
}

void FileFlow::connectReview() {
    connect(m_review, &KeyReview::addClicked, this, &FileFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_review, &KeyReview::backClicked, this, &FileFlow::onBackClicked, qontrol::UNIQUE);
}

void FileFlow::showReview() {
    m_view = View::Review;
    viewSlots()->setHeader(m_review->header());
    viewSlots()->setBody(m_review);
    viewSlots()->setFooter(m_review->actions());
    emit viewChanged(TR("keys-add-file-title"));
}

void FileFlow::hideReview() {
    viewSlots()->keep(m_review);
    viewSlots()->keep(m_review->header());
    viewSlots()->keep(m_review->actions());
}

// ===== Slots =====

void FileFlow::onBackClicked() {
    switch (m_view) {
    case View::Drop:
    case View::Review:
        emit back();
        return;
    }
}

void FileFlow::onNextClicked() {
    switch (m_view) {
    case View::Review:
        if (m_review->add(m_routes)) {
            emit added();
        }
        return;
    case View::Drop:
        return;
    }
}

void FileFlow::onOpenFileClicked() {
    QString path =
        QFileDialog::getOpenFileName(this, TR("keys-pick-file"), QString(), TR("keys-file-filter"));
    if (!path.isEmpty()) {
        loadFile(path);
    }
}

} // namespace keys::modal
