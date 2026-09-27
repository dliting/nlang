// Third-party native library fixture. Compiled to nlang_mylib.dll; the
// matching declarations live in the test-generated mylib.n. Demonstrates
// that a third-party namespace uses the same NativeHost ABI as the
// standard library (math/io/fs).

#include <nlang/vm/NativeHost.h>
#include <string>

using namespace nlang::native;

static void Add(NativeHost* host, uint8_t* ret,
                const uint8_t* args, int argc) {
    (void)host; (void)argc;
    ReturnInt(ret, ArgInt(args, 0) + ArgInt(args, 1));
}

static void Mul(NativeHost* host, uint8_t* ret,
                const uint8_t* args, int argc) {
    (void)host; (void)argc;
    ReturnInt(ret, ArgInt(args, 0) * ArgInt(args, 1));
}

static void Greet(NativeHost* host, uint8_t* ret,
                  const uint8_t* args, int argc) {
    (void)argc;
    std::string who = ArgString(host, args, 0);
    ReturnString(host, ret, std::string("hello, ") + who);
}

NLANG_DEFINE_NATIVE_INIT
{
    reg(registry, "mylib", "add", &Add);
    reg(registry, "mylib", "mul", &Mul);
    reg(registry, "mylib", "greet", &Greet);
    return NLANG_HOST_ABI_VERSION;
}
