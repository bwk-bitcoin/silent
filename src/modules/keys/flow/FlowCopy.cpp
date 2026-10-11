#include "FlowCopy.h"

#include "catalog/Variants.h"
#include "i18n/Tr.h"

namespace keys::flow {

auto flowCopy(Context context) -> FlowCopy {
    switch (context) {
    case Context::Sign:
        return {.title = TR("flow-sign-title"),
                .pickerLead = TR("flow-sign-picker-lead"),
                .usbTitle = TR("flow-usb-confirm-title"),
                .usbWait = TR("flow-sign-usb-wait"),
                .qrTitle = TR("flow-sign-qr-title"),
                .qrHint = TR("flow-sign-qr-hint"),
                .scanTitle = TR("flow-sign-scan-title"),
                .scanHint = TR("flow-sign-scan-hint"),
                .sdTitle = TR("flow-sign-sd-title"),
                .hotPrompt = TR("flow-sign-hot-prompt"),
                .hotBusy = TR("flow-sign-hot-busy")};
    case Context::Verify:
        return {.title = TR("flow-verify-title"),
                .pickerLead = TR("flow-verify-picker-lead"),
                .usbTitle = TR("flow-verify-usb-title"),
                .usbWait = TR("flow-verify-usb-wait"),
                .qrTitle = TR("flow-verify-qr-title"),
                .qrHint = TR("flow-verify-qr-hint"),
                .scanTitle = TR("flow-verify-scan-title"),
                .scanHint = TR("flow-verify-scan-hint"),
                .sdTitle = TR("flow-verify-sd-title"),
                .hotPrompt = TR("flow-verify-hot-prompt"),
                .hotBusy = TR("flow-verify-hot-busy")};
    case Context::Fetch:
        return {.title = TR("flow-fetch-title"),
                .pickerLead = QString(),
                .usbTitle = TR("flow-usb-confirm-title"),
                .usbWait = TR("flow-fetch-usb-wait"),
                .qrTitle = TR("flow-fetch-qr-title"),
                .qrHint = TR("flow-fetch-qr-hint"),
                .scanTitle = TR("flow-fetch-scan-title"),
                .scanHint = TR("flow-fetch-scan-hint"),
                .sdTitle = TR("flow-fetch-sd-title"),
                .hotPrompt = TR("flow-fetch-hot-prompt"),
                .hotBusy = TR("flow-fetch-hot-busy")};
    case Context::FetchXpub:
        return {.title = TR("flow-fetch-xpub-title"),
                .pickerLead = QString(),
                .usbTitle = TR("flow-usb-confirm-title"),
                .usbWait = TR("flow-fetch-xpub-usb-wait"),
                .qrTitle = TR("flow-fetch-xpub-qr-title"),
                .qrHint = TR("flow-fetch-xpub-qr-hint"),
                .scanTitle = TR("flow-fetch-xpub-scan-title"),
                .scanHint = TR("flow-fetch-xpub-scan-hint"),
                .sdTitle = TR("flow-fetch-xpub-title"),
                .hotPrompt = TR("flow-fetch-xpub-hot-prompt"),
                .hotBusy = TR("flow-fetch-xpub-hot-busy")};
    }
    return {};
}

auto hotProceedButton(Context context) -> catalog::Button * {
    switch (context) {
    case Context::Sign:
        return catalog::btnSignHot();
    case Context::Verify:
        return catalog::btnCheckAddress();
    case Context::Fetch:
        return catalog::btnGetDescriptorHot();
    case Context::FetchXpub:
        return catalog::btnGetXpubHot();
    }
    return catalog::btnProceed();
}

} // namespace keys::flow
