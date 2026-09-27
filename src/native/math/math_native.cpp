// math native module — the dynamically-loaded implementation of the `math`
// namespace. Built as nlang_math.dll / libnlang_math.so; the VM loads it
// through the same mechanism as a third-party native library.
//
// This module depends only on the stable NativeHost ABI and the C++ standard
// library — it does not link nlang_vm. The PRNG state is owned by the VM, so
// random/srand/randomi go through the host callbacks instead of keeping a
// local generator.
#include <nlang/vm/NativeHost.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

using nlang::native::ArgFloat;
using nlang::native::ArgInt;
using nlang::native::ReturnFloat;
using nlang::native::ReturnInt;

namespace {

constexpr unsigned kRngFloatShift = 8u;
constexpr float kRngFloatScale = 1.0f / 16777216.0f;  // 2^-24

// Raise the base Exception (math argument/range errors).
void RaiseBase(NativeHost* host, const std::string& msg) {
    nlang::native::Raise(host, NEXC_Base, msg);
}

//--- unary float -> float ---------------------------------------------------
void MathSqrt(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::sqrt(ArgFloat(a, 0)));
}
void MathSin(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::sin(ArgFloat(a, 0)));
}
void MathCos(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::cos(ArgFloat(a, 0)));
}
void MathTan(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::tan(ArgFloat(a, 0)));
}
void MathAsin(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::asin(ArgFloat(a, 0)));
}
void MathAcos(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::acos(ArgFloat(a, 0)));
}
void MathAtan(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::atan(ArgFloat(a, 0)));
}
void MathExp(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::exp(ArgFloat(a, 0)));
}
void MathLog(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::log(ArgFloat(a, 0)));  // natural logarithm
}

//--- binary float -> float --------------------------------------------------
void MathAtan2(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::atan2(ArgFloat(a, 0), ArgFloat(a, 1)));
}
void MathPow(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::pow(ArgFloat(a, 0), ArgFloat(a, 1)));
}

//--- integer / float absolute ----------------------------------------------
void MathAbsi(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    int32_t v = ArgInt(a, 0);
    if (v == INT32_MIN)
        RaiseBase(h, "math.absi: abs of -2147483648 is outside the int32 range.");
    ReturnInt(ret, v < 0 ? -v : v);
}
void MathAbsf(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnFloat(ret, std::fabs(ArgFloat(a, 0)));
}

//--- min / max --------------------------------------------------------------
void MathMini(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    int32_t x = ArgInt(a, 0), y = ArgInt(a, 1);
    ReturnInt(ret, x < y ? x : y);
}
void MathMaxi(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    int32_t x = ArgInt(a, 0), y = ArgInt(a, 1);
    ReturnInt(ret, x > y ? x : y);
}
void MathMinf(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    float x = ArgFloat(a, 0), y = ArgFloat(a, 1);
    ReturnFloat(ret, x < y ? x : y);
}
void MathMaxf(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    float x = ArgFloat(a, 0), y = ArgFloat(a, 1);
    ReturnFloat(ret, x > y ? x : y);
}

//--- clamp ------------------------------------------------------------------
void MathClampi(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    int32_t v = ArgInt(a, 0), lo = ArgInt(a, 1), hi = ArgInt(a, 2);
    if (lo > hi)
        RaiseBase(h, "math.clampi: low is greater than high.");
    ReturnInt(ret, v < lo ? lo : (v > hi ? hi : v));
}
void MathClampf(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    float v = ArgFloat(a, 0), lo = ArgFloat(a, 1), hi = ArgFloat(a, 2);
    if (lo > hi)
        RaiseBase(h, "math.clampf: low is greater than high.");
    ReturnFloat(ret, v < lo ? lo : (v > hi ? hi : v));
}

//--- floor / ceil / round (float -> int) ------------------------------------
int32_t RoundedInt(NativeHost* h, double d, float x, const char* fn) {
    if (!(d >= -2147483648.0 && d <= 2147483647.0)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "math.%s: value %g is outside the int32 range.", fn, x);
        RaiseBase(h, buf);
    }
    return static_cast<int32_t>(d);
}
void MathFloor(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    float x = ArgFloat(a, 0);
    ReturnInt(ret, RoundedInt(h, std::floor(x), x, "floor"));
}
void MathCeil(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    float x = ArgFloat(a, 0);
    ReturnInt(ret, RoundedInt(h, std::ceil(x), x, "ceil"));
}
void MathRound(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    float x = ArgFloat(a, 0);
    ReturnInt(ret, RoundedInt(h, std::round(x), x, "round"));  // half away from zero
}

//--- PRNG (state owned by the VM) ------------------------------------------
void MathRandom(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    float r = static_cast<float>(h->nextRandom(h) >> kRngFloatShift)
            * kRngFloatScale;
    ReturnFloat(ret, r);
}
void MathSrand(NativeHost* h, uint8_t*, const uint8_t* a, int) {
    h->seedRandom(h, ArgInt(a, 0));
}
void MathRandomi(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    int32_t lo = ArgInt(a, 0), hi = ArgInt(a, 1);
    if (lo > hi)
        RaiseBase(h, "math.randomi: min is greater than max.");
    uint32_t span = static_cast<uint32_t>(hi - lo) + 1u;
    int32_t r = lo + static_cast<int32_t>(h->nextRandom(h) % span);
    ReturnInt(ret, r);
}

} // namespace

// Module entry point: register every math function under the "math" namespace.
NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "math", "sqrt",   &MathSqrt);
    reg(registry, "math", "sin",    &MathSin);
    reg(registry, "math", "cos",    &MathCos);
    reg(registry, "math", "tan",    &MathTan);
    reg(registry, "math", "asin",   &MathAsin);
    reg(registry, "math", "acos",   &MathAcos);
    reg(registry, "math", "atan",   &MathAtan);
    reg(registry, "math", "exp",    &MathExp);
    reg(registry, "math", "log",    &MathLog);
    reg(registry, "math", "atan2",  &MathAtan2);
    reg(registry, "math", "pow",    &MathPow);
    reg(registry, "math", "absi",   &MathAbsi);
    reg(registry, "math", "absf",   &MathAbsf);
    reg(registry, "math", "mini",   &MathMini);
    reg(registry, "math", "maxi",   &MathMaxi);
    reg(registry, "math", "minf",   &MathMinf);
    reg(registry, "math", "maxf",   &MathMaxf);
    reg(registry, "math", "clampi", &MathClampi);
    reg(registry, "math", "clampf", &MathClampf);
    reg(registry, "math", "floor",  &MathFloor);
    reg(registry, "math", "ceil",   &MathCeil);
    reg(registry, "math", "round",  &MathRound);
    reg(registry, "math", "random", &MathRandom);
    reg(registry, "math", "srand",  &MathSrand);
    reg(registry, "math", "randomi", &MathRandomi);
    return NLANG_HOST_ABI_VERSION;
}
