#pragma once

#include "theme/Palette.h"
#include <QWidget>

class QHideEvent;
class QPaintEvent;
class QShowEvent;
class QVariantAnimation;

namespace catalog {

// Indeterminate spinner: the refresh glyph in accent, one turn per second while shown.
class Spinner : public QWidget {
    Q_OBJECT

public:
    explicit Spinner(int size = metric::SPINNER_SIZE, QWidget *parent = nullptr);

public slots:
    void onAngleChanged(const QVariant &angle);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QVariantAnimation *m_animation = nullptr;
    qreal m_angle = 0;
    int m_size = metric::SPINNER_SIZE;
};

} // namespace catalog
