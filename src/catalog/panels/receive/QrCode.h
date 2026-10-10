#pragma once

#include <QColor>
#include <QImage>
#include <QWidget>

namespace catalog {

class QrCode : public QWidget {
    Q_OBJECT

public:
    explicit QrCode(QWidget *parent = nullptr);
    // Draws `data` encoded as one QR code.
    void setData(const QString &data);
    void setImage(const QImage &image);
    [[nodiscard]] auto data() const -> QString;
    // A card filled with `background` around the code, instead of the surface card
    // with a white square (the airgap display tunes it for contrast).
    void setBackground(const QColor &background);
    void setPadding(int padding);
    void setRadius(int radius);
    // Fades the code while it is being rebuilt.
    void setDimmed(bool dimmed);

protected:
    void paintEvent(QPaintEvent *event) override;
    [[nodiscard]] auto sizeHint() const -> QSize override;

private:
    QImage m_image;
    QString m_data;
    QColor m_background;
    int m_padding;
    int m_radius;
    bool m_dimmed = false;
};

} // namespace catalog
