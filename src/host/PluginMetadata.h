#pragma once

#include <QByteArray>
#include <QString>
#include <QtTypes>
#include <silent.h>

// The Qt CPU requirements the running host meets: its build's debug bit, with the CPU
// level of the running CPU, not just the one the host was compiled for.
auto hostArchRequirements() -> quint8;

// Empty on admission; expected_iid is explicit so compatibility can be checked both ways.
auto pluginMetadataError(const QByteArray &bytes, const QString &expected_iid) -> QString;

// Seals the plugin bundle at `path` (a ZIP, whatever its name) into the state, its
// library admitted against this Qt host. Blocks on I/O: off the GUI thread.
auto installPluginBundle(const QString &path) -> PluginInstallResult;
