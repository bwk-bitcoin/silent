#pragma once

#include <atomic>
#include <cstdint>

namespace account {

// A request id unique across every view of every account: AccountBackend answers
// on signals all its views listen to, each view keeps only the answers to its ids.
inline auto nextRequestId() -> uint64_t {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1);
}

} // namespace account
