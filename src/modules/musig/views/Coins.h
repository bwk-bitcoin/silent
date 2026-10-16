#pragma once

#include <QString>
#include <Qontrol>
#include <silent.h>

class MusigAccountController;

namespace musig::view {

class Coins : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Coins(MusigAccountController *ctrl);

public slots:
    void onCoinsUpdated(CoinState state);
    void onLabelEdited(const QString &outpoint, const QString &label);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
};

} // namespace musig::view
