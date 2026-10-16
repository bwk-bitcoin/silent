#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <silent.h>

class QLabel;

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig::modal {

// Who signs one path and when, in plain words. `names` are the signer names in
// path order, `mandatory` flags, at the same index, the ones that sign every spend
// (MultiMandatory).
[[nodiscard]] auto pathWords(PolicySemantic semantic, std::uint32_t threshold,
                             PolicyLocktime locktime, std::uint32_t locktime_value,
                             const QStringList &names, const QList<bool> &mandatory) -> QString;

// Step 4: plain words from a PolicyReview, the leaf count and the descriptor.
class ReviewPolicy : public QWidget {
    Q_OBJECT

public:
    explicit ReviewPolicy(QWidget *parent = nullptr);
    void setReview(const PolicyReview &review, const QString &descriptor);

protected:
    void init();
    void view();
    [[nodiscard]] static auto pathText(const PolicyReviewPath &path) -> QString;

private:
    qontrol::Column *m_paths_column = nullptr;
    QLabel *m_leaves_label = nullptr;
    QLabel *m_descriptor_label = nullptr;
};

} // namespace musig::modal
