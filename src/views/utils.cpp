#include "utils.h"
#include "catalog/containers/Card.h"
#include "catalog/display/Label.h"
#include "views/BackendErrors.h"
#include "views/DashboardLayout.h"
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLocale>
#include <QVBoxLayout>
#include <Qontrol>

auto margin(QWidget *widget) -> QWidget * {
    return margin(widget, MARGIN);
}

auto margin(QWidget *widget, int margin) -> QWidget * {
    auto *col = (new qontrol::Column)->pushSpacer(margin)->push(widget)->pushSpacer(margin);
    auto *row = (new qontrol::Row)->pushSpacer(margin)->push(col)->pushSpacer(margin);
    return row;
}

auto dashboard(const QString &title, QWidget *widget, bool two_column, QWidget *title_right)
    -> QWidget * {
    return new DashboardLayout(title, widget, two_column, nullptr, title_right);
}

auto frame(QWidget *widget) -> QWidget * {
    auto *card = new catalog::Card;
    card->setContent(widget);
    return card;
}

void setScreenContent(QWidget *screen, QWidget *&main_widget, QWidget *content) {
    auto *outer = qobject_cast<QVBoxLayout *>(screen->layout());
    if (outer == nullptr) {
        outer = new QVBoxLayout(screen);
        outer->setContentsMargins(0, 0, 0, 0);
    }
    delete main_widget;
    main_widget = content;
    outer->addWidget(content, 1); // fill the screen
}

auto shortenOutpoint(const QString &outpoint) -> QString {
    int colonIdx = outpoint.lastIndexOf(':');
    if (colonIdx < 12) {
        return outpoint;
    }
    QString txid = outpoint.left(colonIdx);
    QString vout = outpoint.mid(colonIdx);
    return txid.left(6) + "..." + txid.right(6) + vout;
}

auto coinsCount(uint64_t count) -> QString {
    auto coinsStr = QLocale().toString(static_cast<qulonglong>(count));
    return QCoreApplication::translate("view::utils", "%1 coins").arg(coinsStr);
}

auto mapBackendErrorSummary(BackendErrorKind kind) -> QString {
    switch (kind) {
    case BackendErrorKind::InputConflict:
        return QCoreApplication::translate("view::utils", "Transaction input conflict detected.");
    case BackendErrorKind::AddressReuse:
        return QCoreApplication::translate("view::utils", "Address reuse detected.");
    case BackendErrorKind::Network:
        return QCoreApplication::translate("view::utils",
                                           "Network connection failed. Check endpoint settings.");
    case BackendErrorKind::Electrum:
        return QCoreApplication::translate("view::utils", "Electrum connection failed.");
    case BackendErrorKind::Backend:
        return QCoreApplication::translate("view::utils", "Backend request failed.");
    case BackendErrorKind::Signing:
        return QCoreApplication::translate("view::utils", "Failed to sign transaction.");
    case BackendErrorKind::Broadcast:
        return QCoreApplication::translate("view::utils", "Failed to broadcast transaction.");
    default:
        return QCoreApplication::translate("view::utils", "Operation failed.");
    }
}

auto operationFailedSummary() -> QString {
    return mapBackendErrorSummary(BackendErrorKind::Other);
}

auto formatBackendErrorDetails(const QString &raw_error) -> QString {
    return QCoreApplication::translate("view::utils", "Details: %1").arg(raw_error.trimmed());
}
