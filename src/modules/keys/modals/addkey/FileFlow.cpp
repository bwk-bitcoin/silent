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
#include <QFileDialog>
#include <QLabel>
#include <QMimeData>
#include <QThread>
#include <QUrl>
#include <optional>

#include <Qontrol>
#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

FileFlow::FileFlow(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
}

FileFlow::~FileFlow() {
    if (m_worker != nullptr) {
        m_worker->wait();
    }
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
    connect(this, &FileFlow::previewReady, this, &FileFlow::onPreviewReady, qontrol::UNIQUE);
    connect(this, &FileFlow::commitReady, this, &FileFlow::onCommitReady, qontrol::UNIQUE);
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
    if (m_worker != nullptr) {
        return;
    }
    m_read_error->setVisible(false);
    m_file_error->setVisible(false);
    m_network = KeysModule::network();
    m_preview = std::make_shared<std::optional<rust::Box<KeyFilePreview>>>();
    auto preview = m_preview;
    m_worker = QThread::create([this, preview, path = path.toStdString(), network = m_network] {
        preview->emplace(::key_file_preview(path, network));
        emit previewReady();
    });
    connect(m_worker, &QThread::finished, m_worker, &QThread::deleteLater, qontrol::UNIQUE);
    setEnabled(false);
    m_worker->start();
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
    if (m_worker != nullptr) {
        return;
    }
    switch (m_view) {
    case View::Drop:
    case View::Review:
        emit back();
        return;
    }
}

void FileFlow::onNextClicked() {
    if (m_worker != nullptr) {
        return;
    }
    switch (m_view) {
    case View::Review:
        m_commit = std::make_shared<std::optional<rust::Box<KeyFileCommit>>>();
        m_review->setBusy(true);
        emit busyChanged(true);
        m_worker = QThread::create([this, preview = m_preview, committed = m_commit,
                                    alias = m_review->alias().toStdString()] {
            committed->emplace(::commit_key_file(*preview->value(), alias));
            emit commitReady();
        });
        connect(m_worker, &QThread::finished, m_worker, &QThread::deleteLater, qontrol::UNIQUE);
        m_worker->start();
        return;
    case View::Drop:
        return;
    }
}

void FileFlow::onPreviewReady() {
    m_worker->wait();
    m_worker = nullptr;
    setEnabled(true);
    auto &preview = m_preview->value();
    if (!preview->is_ok()) {
        switch (preview->file_failure()) {
        case KeyFileFailure::Read:
            m_read_error->setVisible(true);
            break;
        case KeyFileFailure::Newer:
            showFileError(TR("keys-file-newer"));
            break;
        case KeyFileFailure::Network:
            showFileError(TR("keys-file-other-network")
                              .arg(networkLabel(preview->file_network()), networkLabel(m_network)));
            break;
        default:
            showFileError(TR("keys-file-invalid") + QStringLiteral("\n") +
                          KeysModule::toQt(preview->get_error()));
            break;
        }
        return;
    }
    QList<PathXpub> xpubs;
    for (const auto &entry : preview->file_xpubs()) {
        xpubs.append({.path = KeysModule::toQt(entry.path), .xpub = KeysModule::toQt(entry.xpub)});
    }
    m_review->setKey(KeysModule::toQt(preview->file_fingerprint()), xpubs,
                     KeysModule::toQt(preview->file_alias()));
    showReview();
}

void FileFlow::onCommitReady() {
    m_worker->wait();
    m_worker = nullptr;
    m_review->setBusy(false);
    emit busyChanged(false);
    auto &committed = m_commit->value();
    if (!committed->is_ok()) {
        m_review->showError(TR("keys-add-failed").arg(KeysModule::toQt(committed->get_error())));
        return;
    }
    auto applied = m_module->applyKeyFile(m_network, *committed);
    if (!applied.is_ok) {
        m_review->showError(TR("keys-add-failed").arg(KeysModule::toQt(applied.error)));
        return;
    }
    emit added();
}

void FileFlow::onOpenFileClicked() {
    QString path =
        QFileDialog::getOpenFileName(this, TR("keys-pick-file"), QString(), TR("keys-file-filter"));
    if (!path.isEmpty()) {
        loadFile(path);
    }
}

} // namespace keys::modal
