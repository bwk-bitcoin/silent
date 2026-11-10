#pragma once

#include <optional>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <theme/Palette.h>

#include <interfaces/types.h>

class IThemeProvider : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IThemeProvider() override = default;

    [[nodiscard]] virtual bool implemented() const = 0;
    virtual ReqId themes() = 0;
    virtual ReqId palette(const QString &name) = 0;
    virtual ReqId raw(const QByteArray &request) = 0;

signals:
    void themes(ReqId req_id, QStringList themes);
    void palette(ReqId req_id, theme::Palette palette);
    void raw(ReqId req_id, QByteArray response);
    void error(std::optional<ReqId> req_id, QString message);
};

Q_DECLARE_INTERFACE(IThemeProvider, "dev.silent.IThemeProvider/1.0")
