#include "KeyFields.h"

#include "catalog/feedback/Notice.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"

namespace keys::modal {

auto aliasInput() -> catalog::Input * {
    auto *input = new catalog::Input;
    input->setPlaceholderText(TR("keys-alias-name"));
    return input;
}

auto duplicateNotice() -> catalog::Notice * {
    auto *notice = new catalog::Notice(catalog::Notice::Tone::Error);
    notice->setTitle(TR("keys-duplicate-title"));
    notice->setVisible(false);
    return notice;
}

auto errorNotice() -> catalog::Notice * {
    auto *notice = new catalog::Notice(catalog::Notice::Tone::Error);
    notice->setVisible(false);
    return notice;
}

} // namespace keys::modal
