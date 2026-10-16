#include "modules/musig/creator/ReviewPolicy.h"

namespace musig::modal {

ReviewPolicy::ReviewPolicy(QWidget *parent) : QWidget(parent) {
}

void ReviewPolicy::setReview(const PolicyReview &review, const QString &descriptor) {
    Q_UNUSED(review);
    Q_UNUSED(descriptor);
}

void ReviewPolicy::init() {
}

void ReviewPolicy::view() {
}

auto ReviewPolicy::pathText(const PolicyReviewPath &path) -> QString {
    Q_UNUSED(path);
    return {};
}

} // namespace musig::modal
