#pragma once

namespace catalog {
class Input;
class Notice;
} // namespace catalog

namespace keys::modal {

// The fields and notices the add views share.
[[nodiscard]] auto aliasInput() -> catalog::Input *;
[[nodiscard]] auto duplicateNotice() -> catalog::Notice *;
[[nodiscard]] auto errorNotice() -> catalog::Notice *;

} // namespace keys::modal
