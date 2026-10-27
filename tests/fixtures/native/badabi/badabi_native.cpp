// Bad-ABI fixture: reports an incompatible ABI version so the loader must
// reject it. Hand-written on purpose — NLANG_DEFINE_NATIVE_INIT now emits
// a correct nlang_abi_version probe, while this fixture models a module
// built against a DIFFERENT host ABI, where the probe itself is wrong.
// Its init also registers a function first, the way a real stale module
// would: a loader that validated the version only after init would leak
// that registration (a pointer into a module about to be unloaded).

#include "nlang/vm/NativeHost.h"

namespace {
void DeadRegistration(NativeHost*, uint8_t*, const uint8_t*, int) {}
}

NLANG_NATIVE_EXPORT unsigned nlang_abi_version(void) { return 999999u; }

NLANG_NATIVE_EXPORT int nlang_native_init(void* registry,
                                          RegisterNativeFn reg,
                                          RegisterNativeBlockingFn) {
    reg(registry, "badabi", "dead", &DeadRegistration);
    return 999999;
}
