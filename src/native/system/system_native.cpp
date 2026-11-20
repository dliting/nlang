// system native module — the dynamically-loaded implementation of the
// `system` namespace. Built as nlang_system.dll / libnlang_system.so;
// the VM loads it through the same mechanism as a third-party native
// library.
//
// This module depends only on the stable NativeHost ABI and the C++
// standard library — it does not link nlang_vm.
#include <nlang/vm/NativeHost.h>

#include <chrono>
#include <cstdint>

using nlang::native::ReturnLong;

namespace {

// wallMs: the host's wall clock in Unix epoch milliseconds.
void SystemWallMs(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    (void)h;
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ReturnLong(ret, static_cast<int64_t>(ms));
}

// monoMs: the host's monotonic millisecond clock — the same clock
// domain as parkForIo deadlines (a deadline is an absolute monoMs
// value), so interval arithmetic stays consistent across the two.
void SystemMonoMs(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    ReturnLong(ret, h->nowMs(h));
}

} // namespace

NLANG_DEFINE_NATIVE_INIT {
    (void)regBlocking;
    reg(registry, "system", "wallMs", &SystemWallMs);
    reg(registry, "system", "monoMs", &SystemMonoMs);
    return NLANG_HOST_ABI_VERSION;
}
