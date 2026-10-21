#pragma once

#include <QByteArray>
#include <QString>

// Empty on admission; expected_iid is explicit so compatibility can be checked both ways.
auto pluginMetadataError(const QByteArray &bytes, const QString &expected_iid) -> QString;
