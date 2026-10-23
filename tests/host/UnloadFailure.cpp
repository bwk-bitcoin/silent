#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>

// Fail one actual memfd dlclose without unmapping it. All other loader calls pass
// through, so the host must retain its mapped loader and retry without stale roots.
extern "C" auto dlclose(void *handle) noexcept -> int {
    using Close = int (*)(void *);
    static auto close = reinterpret_cast<Close>(dlsym(RTLD_NEXT, "dlclose"));
    static std::atomic_bool failed = false;
    link_map *mapping = nullptr;
    if (std::getenv("SILENT_EXPECT_UNLOAD_FAILURE") != nullptr &&
        dlinfo(handle, RTLD_DI_LINKMAP, &mapping) == 0 && mapping != nullptr &&
        std::strncmp(mapping->l_name, "/proc/self/fd/", 14) == 0 && !failed.exchange(true)) {
        return -1;
    }
    return close(handle);
}
