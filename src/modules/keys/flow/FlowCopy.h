#pragma once

#include <QString>

#include <cstdint>

namespace catalog {
class Button;
}

namespace keys::flow {

// What a signing flow was opened for: it sets every title and line the steps show.
// Fetch asks the key for its sp() descriptor, FetchXpub for its xpub at a path.
enum class Context : std::uint8_t { Sign, Verify, Fetch, FetchXpub };

// The copy of one context. `usbWait` takes the device name, `hotPrompt` the key alias.
struct FlowCopy {
    QString title;
    QString pickerLead;
    QString usbTitle;
    QString usbWait;
    QString qrTitle;
    QString qrHint;
    QString scanTitle;
    QString scanHint;
    QString sdTitle;
    QString hotPrompt;
    QString hotBusy;
};

auto flowCopy(Context context) -> FlowCopy;
// The hot key step's proceed button for `context`.
auto hotProceedButton(Context context) -> catalog::Button *;

} // namespace keys::flow
