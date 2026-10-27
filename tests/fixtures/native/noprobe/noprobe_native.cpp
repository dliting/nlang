// No-probe fixture: a hand-written module that exports nlang_native_init
// (registering one function, returning a correct version) but NOT the
// required nlang_abi_version probe. The loader must reject it by name
// before any init code runs — without the probe check this module loads
// and registers successfully, which is exactly the regression the
// MissingAbiProbe test pins.

#include "nlang/vm/NativeHost.h"

namespace {
void ProbelessFunction(NativeHost*, uint8_t*, const uint8_t*, int) {}
}

NLANG_NATIVE_EXPORT int nlang_native_init(void* registry,
                                          RegisterNativeFn reg,
                                          RegisterNativeBlockingFn) {
    reg(registry, "noprobe", "fn", &ProbelessFunction);
    return static_cast<int>(NLANG_HOST_ABI_VERSION);
}
