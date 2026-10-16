#pragma once

#include <Qontrol>
#include <silent.h>

class MusigAccountController;

namespace musig::view {

class History : public qontrol::Screen {
    Q_OBJECT

public:
    explicit History(MusigAccountController *ctrl);

public slots:
    void onCoinsUpdated(CoinState state);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
};

} // namespace musig::view
