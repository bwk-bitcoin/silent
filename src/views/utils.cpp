#include "utils.h"
#include "catalog/containers/Card.h"
#include "catalog/display/Label.h"
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

auto mapBackendErrorSummary(const QString &raw_error) -> QString {
    auto err = raw_error.trimmed().toLower();
    if (err.contains("already spent") || err.contains("double-spend") || err.contains("spent")) {
        return QCoreApplication::translate("view::utils", "Transaction input conflict detected.");
    }
    if (err.contains("address reuse") || err.contains("reuse")) {
        return QCoreApplication::translate("view::utils", "Address reuse detected.");
    }
    if (err.contains("timeout") || err.contains("dns") || err.contains("socket") ||
        err.contains("connect") || err.contains("connection") || err.contains("network")) {
        return QCoreApplication::translate("view::utils",
                                           "Network connection failed. Check endpoint settings.");
    }
    if (err.contains("electrum")) {
        return QCoreApplication::translate("view::utils", "Electrum connection failed.");
    }
    if (err.contains("blindbit") || err.contains("backend")) {
        return QCoreApplication::translate("view::utils", "Backend request failed.");
    }
    if (err.contains("sign") || err.contains("signature")) {
        return QCoreApplication::translate("view::utils", "Failed to sign transaction.");
    }
    if (err.contains("broadcast") || err.contains("mempool")) {
        return QCoreApplication::translate("view::utils", "Failed to broadcast transaction.");
    }
    return QCoreApplication::translate("view::utils", "Operation failed.");
}

auto formatBackendErrorDetails(const QString &raw_error) -> QString {
    return QCoreApplication::translate("view::utils", "Details: %1").arg(raw_error.trimmed());
}
