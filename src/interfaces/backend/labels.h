#pragma once

#include <QList>
#include <QString>

#include "service.h"
#include "types.h"

namespace backend {

// The account's labels on its addresses, coins and transactions.
class ILabels : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~ILabels() override = default;

    // Every label the account holds, edits not yet saved included. Changes with
    // labelsChanged().
    [[nodiscard]] virtual auto labels() const -> QList<Label> = 0;

    // The text of the label on `key`, empty when it has none.
    [[nodiscard]] virtual auto label(const LabelKey &key) const -> QString = 0;

public slots:
    // Sets the label on `key` to `text` and saves it, an empty text removing it: answered by
    // saved(). In a built-in account, a save failing after the edit is held shows the edit
    // (labelsChanged()) and answers error().
    virtual void setLabel(backend::RequestId request_id, backend::LabelKey key, QString text) = 0;

signals:
    // labels() changed.
    void labelsChanged();
    // Answers setLabel(): the label is saved.
    void saved(backend::RequestId request_id);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::ILabels, "dev.silent.backend.ILabels/1.0")
