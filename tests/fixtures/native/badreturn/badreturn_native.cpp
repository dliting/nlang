// Wrong-init-return fixture: the probe is macro-emitted (correct), but
// nlang_native_init returns a deliberately wrong constant. The probe is
// the single version authority and the init return value is purely
// informational, so this module must LOAD and its registration must
// reach the host (TestInitReturnInformational pins that a refactor
// re-checking the init return would fail here).

#include "nlang/vm/NativeHost.h"

namespace {
void WrongReturnFunction(NativeHost*, uint8_t*, const uint8_t*, int) {}
}

NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "badreturn", "fn", &WrongReturnFunction);
    return 999999;
}
