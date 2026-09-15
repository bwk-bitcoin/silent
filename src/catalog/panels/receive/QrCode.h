#pragma once

#include <QImage>
#include <QWidget>

namespace catalog {

class QrCode : public QWidget {
    Q_OBJECT

public:
    explicit QrCode(QWidget *parent = nullptr);
    void setData(const QString &data);
    void setImage(const QImage &image);
    [[nodiscard]] auto data() const -> QString;

protected:
    void paintEvent(QPaintEvent *event) override;
    [[nodiscard]] auto sizeHint() const -> QSize override;

private:
    QImage m_image;
    QString m_data;
};

} // namespace catalog
