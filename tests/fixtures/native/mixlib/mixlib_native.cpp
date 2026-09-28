// Mixed third-party library fixture: the native side of "mixlib".
// Compiled to nlang_mixlib.dll; the matching declarations (including the
// NLang wrapper `quad`) live in mixlib.n. See test_thirdparty.cpp.

#include <nlang/vm/NativeHost.h>

using namespace nlang::native;

static void Dbl(NativeHost* host, uint8_t* ret,
                const uint8_t* args, int argc) {
    (void)host; (void)argc;
    ReturnInt(ret, ArgInt(args, 0) * 2);
}

NLANG_DEFINE_NATIVE_INIT
{
    reg(registry, "mixlib", "dbl", &Dbl);
    return NLANG_HOST_ABI_VERSION;
}
