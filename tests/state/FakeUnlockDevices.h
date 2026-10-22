#pragma once

#include <atomic>

namespace setup_test {

inline int discoveries = 0;
inline int releases = 0;
inline int deliveries = 0;
inline int requests = 0;
inline bool listDevice = false;
inline bool refuseRequest = false;
inline std::atomic<int> pending = 0;

} // namespace setup_test
