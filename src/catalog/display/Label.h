#pragma once

#include <QLabel>
#include <QString>
#include <optional>

struct Palette;

namespace catalog {

enum class LabelRole {
    Body,          // Default body text
    Title,         // Page/screen titles
    Heading,       // Section headings
    Subheading,    // Subsection headings
    Section,       // Group/separator labels
    InputLabel,    // Labels preceding text inputs
    CheckboxLabel, // Labels for checkboxes
    InfoLabel,     // Labels preceding read-only values
    Info,          // Read-only values paired with InfoLabel
    Caption,       // Fine print, indicators
    Mono,          // Addresses, amounts, outpoints
    Status,        // Status bar messages
    Eyebrow,       // Uppercase mono section labels
};

// Text color overriding the role's. Inverse tones sit on the fixed dark camera
// surface, whatever the theme.
enum class LabelTone {
    Default,
    Secondary,
    Muted,
    Accent,
    Success,
    Warning,
    Error,
    OnAccent,
    Inverse,
    InverseMuted
};

// Font size step overriding the role's: Sm caption, Md body, Lg section.
enum class LabelSize { Sm, Md, Lg };

class Label : public QLabel {
    Q_OBJECT

public:
    explicit Label(const QString &text, LabelRole role = LabelRole::Body,
                   QWidget *parent = nullptr);
    explicit Label(LabelRole role = LabelRole::Body, QWidget *parent = nullptr);
    void setRole(LabelRole role);
    [[nodiscard]] auto role() const -> LabelRole;
    void setScale(qreal scale);
    [[nodiscard]] auto scale() const -> qreal;
    void setTone(LabelTone tone);
    void setSize(LabelSize size);
    // Wraps `text` at any character, not only at spaces (long mono values).
    void setTextWrapAnywhere(const QString &text);
    // Wraps `text` at `max_width` only when it does not fit on one line, and then sizes
    // the label to it: a wrapped label left to size itself picks a narrow width and,
    // aligned in a layout, loses lines.
    void setTextWrapped(const QString &text, int max_width);
    static auto qss(const Palette &p) -> QString;

protected:
    void applyRole();

private:
    LabelRole m_role = LabelRole::Body;
    qreal m_scale = 1.0;
    std::optional<LabelSize> m_size;
};

} // namespace catalog
