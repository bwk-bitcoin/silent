#pragma once

#include <QList>
#include <QString>

#include "service.h"
#include "types.h"

namespace backend {

class ILabels : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~ILabels() override = default;

    [[nodiscard]] virtual auto labels() const -> QList<Label> = 0;

    [[nodiscard]] virtual auto label(const LabelKey &key) const -> QString = 0;

public slots:
    virtual void setLabel(backend::RequestId request_id, backend::LabelKey key, QString text) = 0;

signals:
    void labelsChanged();
    void saved(backend::RequestId request_id);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::ILabels, "dev.silent.backend.ILabels/1.0")
