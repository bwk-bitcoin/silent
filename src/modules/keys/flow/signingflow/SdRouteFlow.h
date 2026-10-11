#pragma once

#include <QString>

#include <cstdint>

#include "modules/keys/flow/SigningFlow.h"

#include <interfaces/types.h>

namespace catalog {
class Button;
class ModalActions;
class Notice;
class SelectRow;
} // namespace catalog

namespace keys::flow {

// Signs over an SD card, in two rows: save the unsigned transaction, then load the
// signed one back. A failed save or load is reported under the rows, with a retry.
class SdRouteFlow final : public SigningRouteFlow {
    Q_OBJECT

public:
    SdRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row, QString psbt);
    // Shows the save row.
    void start() override;

public slots:
    void onNextClicked();
    void onSdSaved(ReqId req_id, bool is_ok, const QString &error);
    void onSdLoaded(ReqId req_id, bool is_ok, const QString &psbt_hex, const QString &error);

protected:
    enum class SdPhase : std::uint8_t { Save, Load };
    enum class SdFailure : std::uint8_t { None, Write, Read };

    void init();
    void doConnect();
    void view();

    // SD card view
    void initSd();
    void connectSd();
    [[nodiscard]] auto viewSd() -> QWidget *;
    void showSd();
    void refreshSd();
    void saveSd();
    void loadSd();

private:
    ReqId m_sd_req = 0;
    // The folder of the last file picked, where the next file dialog opens.
    QString m_sd_dir;
    SdPhase m_sd_phase = SdPhase::Save;
    SdFailure m_sd_failure = SdFailure::None;
    QWidget *m_sd_step = nullptr;
    catalog::SelectRow *m_sd_save_row = nullptr;
    catalog::SelectRow *m_sd_load_row = nullptr;
    catalog::Notice *m_sd_write_error = nullptr;
    catalog::Notice *m_sd_read_error = nullptr;
    catalog::ModalActions *m_sd_actions = nullptr;
    catalog::Button *m_sd_save = nullptr;
    catalog::Button *m_sd_save_retry = nullptr;
    catalog::Button *m_sd_load = nullptr;
    catalog::Button *m_sd_load_retry = nullptr;
};

} // namespace keys::flow
