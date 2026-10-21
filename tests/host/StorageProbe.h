#pragma once

#include "views/modals/PluginAccessPrompt.h"
#include <QApplication>
#include <QObject>
#include <QString>
#include <QWidget>

namespace smoke {

// Answers Deny to every Saved Data prompt the host opens.
class StoragePrompts final : public QObject {
    Q_OBJECT

public slots:

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void onTick() {
        for (auto *widget : QApplication::topLevelWidgets()) {
            auto *prompt = qobject_cast<modal::PluginAccessPrompt *>(widget);
            if (prompt != nullptr && prompt->isVisible() && !prompt->answered()) {
                prompt->onDenyClicked();
            }
        }
    }
};

auto storageSmoke(const QString &password) -> bool;

} // namespace smoke
