#pragma once

#include <QString>
#include <QWidget>

#include <cstdint>

namespace catalog {
class Button;
class ModalActions;
class Notice;
class SelectRow;
} // namespace catalog

namespace keys::flow {

// Signing over an SD card in two rows: save the unsigned transaction, then load the
// signed one back. A failed save or load is reported under the rows, with a retry.
class SdStep : public QWidget {
    Q_OBJECT

public:
    enum class Phase : std::uint8_t { Save, Load };

    explicit SdStep(QWidget *parent = nullptr);
    // `file_base` names both files: `{base}-unsigned.psbt` and `{base}-signed.psbt`.
    void setFileBase(const QString &file_base);
    void setPhase(Phase phase);
    void showWriteError();
    void showReadError();
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void saveClicked();
    void loadClicked();

protected:
    void init();
    void doConnect();
    void view();
    void refresh();

private:
    // Which file operation failed last, cleared by a phase change.
    enum class Failure : std::uint8_t { None, Write, Read };

    QString m_file_base;
    Phase m_phase = Phase::Save;
    Failure m_failure = Failure::None;
    catalog::SelectRow *m_save_row = nullptr;
    catalog::SelectRow *m_load_row = nullptr;
    catalog::Notice *m_write_error = nullptr;
    catalog::Notice *m_read_error = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_save = nullptr;
    catalog::Button *m_save_retry = nullptr;
    catalog::Button *m_load = nullptr;
    catalog::Button *m_load_retry = nullptr;
};

} // namespace keys::flow
