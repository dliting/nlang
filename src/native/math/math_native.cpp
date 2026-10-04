// math native module — the dynamically-loaded implementation of the `math`
// namespace. Built as nlang_math.dll / libnlang_math.so; the VM loads it
// through the same mechanism as a third-party native library.
//
// This module depends only on the stable NativeHost ABI and the C++ standard
// library — it does not link nlang_vm. The PRNG state is owned by the VM, so
// random/srand/randomi go through the host callbacks instead of keeping a
// local generator.
//
// Error model: argument/range errors (clampi lo>hi, randomi min>max) and
// int64-overflow guards (floor/ceil/round out of range or NaN, absi of
// INT_MIN) raise the BASE Exception class. Domain errors of the
// transcendental family (sqrt of negatives, log of non-positives, asin
// outside [-1,1]) deliberately propagate NaN per C semantics.
//
// The float-typed family runs at double precision: arguments are read as
// 8-byte doubles and results written back the same way.
#include <nlang/vm/NativeHost.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

using nlang::native::ArgDouble;
using nlang::native::ArgInt;
using nlang::native::ReturnDouble;
using nlang::native::ReturnInt;
using nlang::native::ReturnLong;

namespace {

//PRNG granularity: (rng() >> 8) is a 24-bit value; scaling by 2^-24 maps
//it exactly onto [0,1) with no rounding. The double carrier widens the
//presentation only — one draw still yields 2^24 distinct values.
constexpr unsigned kRngFloatShift = 8u;
constexpr double kRngFloatScale = 1.0 / 16777216.0;

//int64 conversion bounds as doubles: -2^63 is exactly representable,
//2^63 itself is one past int64 max (2^63-1 is not representable).
constexpr double kInt64MinAsDouble = -9223372036854775808.0;
constexpr double kInt64EndAsDouble = 9223372036854775808.0;

// Raise the base Exception (math argument/range errors).
void RaiseBase(NativeHost* host, const std::string& msg) {
    nlang::native::Raise(host, NEXC_Base, msg);
}

//--- unary double -> double --------------------------------------------------
void MathSqrt(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::sqrt(ArgDouble(a, 0)));
}
void MathSin(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::sin(ArgDouble(a, 0)));
}
void MathCos(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::cos(ArgDouble(a, 0)));
}
void MathTan(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::tan(ArgDouble(a, 0)));
}
void MathAsin(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::asin(ArgDouble(a, 0)));
}
void MathAcos(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::acos(ArgDouble(a, 0)));
}
void MathAtan(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::atan(ArgDouble(a, 0)));
}
void MathExp(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::exp(ArgDouble(a, 0)));
}
void MathLog(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::log(ArgDouble(a, 0)));  // natural logarithm
}

//--- binary double -> double -------------------------------------------------
void MathAtan2(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::atan2(ArgDouble(a, 0), ArgDouble(a, 1)));
}
void MathPow(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::pow(ArgDouble(a, 0), ArgDouble(a, 1)));
}

//--- integer / double absolute ----------------------------------------------
void MathAbsi(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    int32_t v = ArgInt(a, 0);
    if (v == INT32_MIN)
        RaiseBase(h, "math.absi: abs of -2147483648 is outside the int32 range.");
    ReturnInt(ret, v < 0 ? -v : v);
}
void MathAbsf(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    ReturnDouble(ret, std::fabs(ArgDouble(a, 0)));
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
    double x = ArgDouble(a, 0), y = ArgDouble(a, 1);
    ReturnDouble(ret, x < y ? x : y);
}
void MathMaxf(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    double x = ArgDouble(a, 0), y = ArgDouble(a, 1);
    ReturnDouble(ret, x > y ? x : y);
}

//--- clamp ------------------------------------------------------------------
void MathClampi(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    int32_t v = ArgInt(a, 0), lo = ArgInt(a, 1), hi = ArgInt(a, 2);
    if (lo > hi)
        RaiseBase(h, "math.clampi: low is greater than high.");
    ReturnInt(ret, v < lo ? lo : (v > hi ? hi : v));
}
void MathClampf(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    double v = ArgDouble(a, 0), lo = ArgDouble(a, 1), hi = ArgDouble(a, 2);
    if (lo > hi)
        RaiseBase(h, "math.clampf: low is greater than high.");
    ReturnDouble(ret, v < lo ? lo : (v > hi ? hi : v));
}

//--- floor / ceil / round (double -> long) -----------------------------------
int64_t RoundedLong(NativeHost* h, double d, double x, const char* fn) {
    //C++ double->int64 conversion outside [-2^63, 2^63) is UB (x86
    //silently yields 0x80000000...), so reject loudly before casting.
    //NaN fails the same comparison and lands in the same error.
    if (!(d >= kInt64MinAsDouble && d < kInt64EndAsDouble)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "math.%s: value %g is outside the int64 range.", fn, x);
        RaiseBase(h, buf);
    }
    return static_cast<int64_t>(d);
}
void MathFloor(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    double x = ArgDouble(a, 0);
    ReturnLong(ret, RoundedLong(h, std::floor(x), x, "floor"));
}
void MathCeil(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    double x = ArgDouble(a, 0);
    ReturnLong(ret, RoundedLong(h, std::ceil(x), x, "ceil"));
}
void MathRound(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    double x = ArgDouble(a, 0);
    //half away from zero
    ReturnLong(ret, RoundedLong(h, std::round(x), x, "round"));
}

//--- PRNG (state owned by the VM) ------------------------------------------
void MathRandom(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    double r = static_cast<double>(h->nextRandom(h) >> kRngFloatShift)
             * kRngFloatScale;
    ReturnDouble(ret, r);
}
void MathSrand(NativeHost* h, uint8_t*, const uint8_t* a, int) {
    h->seedRandom(h, ArgInt(a, 0));
}
void MathRandomi(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    int32_t lo = ArgInt(a, 0), hi = ArgInt(a, 1);
    if (lo > hi)
        RaiseBase(h, "math.randomi: min is greater than max.");
    //Modulo bias exists for ranges not dividing 2^32; documented as
    //acceptable for a scripting PRNG (deterministic, no adapter).
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
