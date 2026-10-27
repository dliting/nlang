// No-entry fixture: a shared library that deliberately does NOT export
// nlang_native_init, so the loader must report the missing entry point.
// It DOES export a correct nlang_abi_version probe: version and entry are
// validated in that order, so this fixture pins the missing-entry error
// (a probe-less variant is covered by the noprobe fixture).

#include "nlang/vm/NativeHost.h"

NLANG_NATIVE_EXPORT unsigned nlang_abi_version(void) {
    return NLANG_HOST_ABI_VERSION;
}

void some_unrelated_symbol() {}
