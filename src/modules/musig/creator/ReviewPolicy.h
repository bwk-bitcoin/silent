#pragma once

#include <QString>
#include <QWidget>
#include <silent.h>

class QLabel;

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig::modal {

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
