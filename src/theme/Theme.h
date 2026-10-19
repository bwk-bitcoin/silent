#pragma once

#include "Palette.h"
#include <QObject>

enum class ThemeMode { Light, Dark };

class Theme : public QObject {
    Q_OBJECT

public:
    static void init();
    static auto get() -> Theme *;
    void setMode(ThemeMode mode);
    void setPalette(const Palette &palette);
    [[nodiscard]] auto mode() const -> ThemeMode;
    [[nodiscard]] auto palette() const -> const Palette &;
    [[nodiscard]] auto fontPalette() const -> const FontPalette &;
    [[nodiscard]] auto buttonPalette() const -> const ButtonPalette &;
    [[nodiscard]] auto iconPalette() const -> const IconPalette &;
    [[nodiscard]] auto inputPalette() const -> const InputPalette &;

    void apply();

signals:
    void themeChanged(ThemeMode mode);

private:
    explicit Theme(QObject *parent = nullptr);
    static Theme *s_instance; // NOLINT(readability-identifier-naming)
    // Light, the default theme: the app state, which holds the theme picked, is read
    // only once unlocked, so the screens before it use this.
    ThemeMode m_mode = ThemeMode::Light;
    Palette m_palette = Palette::light();
    FontPalette m_font_palette = FontPalette::light();
    ButtonPalette m_button_palette = ButtonPalette::light();
    IconPalette m_icon_palette = IconPalette::light();
    InputPalette m_input_palette = InputPalette::light();
};
