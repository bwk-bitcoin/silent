#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> armed{false};
std::atomic<bool> held{false};
std::atomic<bool> released{false};
std::atomic<bool> appState{false};
} // namespace

extern "C" void silentTestFsyncArm() {
    appState.store(false);
    released.store(false);
    held.store(false);
    armed.store(true);
}

extern "C" void silentTestAppStateFsyncArm() {
    appState.store(true);
    released.store(false);
    held.store(false);
    armed.store(true);
}

extern "C" auto silentTestFsyncHeld() -> bool {
    return held.load();
}

extern "C" void silentTestFsyncRelease() {
    released.store(true);
}

// Only the opted-in test process loads this shim. Hold the selected temp's sync,
// after Rust wrote the ciphertext and while it still owns its publication locks.
extern "C" auto fsync(int fd) -> int {
    using Sync = int (*)(int);
    static auto realSync = reinterpret_cast<Sync>(dlsym(RTLD_NEXT, "fsync"));
    if (realSync == nullptr) {
        _exit(125);
    }
    char descriptor[64];
    auto count = std::snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    char target[4096];
    auto length = count > 0 ? readlink(descriptor, target, sizeof(target) - 1) : -1;
    if (length > 0) {
        target[length] = '\0';
        auto *name = std::strrchr(target, '/');
        const auto *targetName = appState.load() ? "/app.tmp" : "/tree.tmp";
        if (name != nullptr && std::strcmp(name, targetName) == 0 && armed.exchange(false)) {
            held.store(true);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!released.load()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    _exit(124);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    return realSync(fd);
}
