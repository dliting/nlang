// Sample native module fixture for NativeLibraryLoader tests. Registers a
// few functions in namespace "sample" covering int, float and string
// arguments/returns.

#include "nlang/vm/NativeHost.h"

#include <string>

using namespace nlang;

void sample_add(NativeHost* host, uint8_t* ret, const uint8_t* args,
                int /*argc*/) {
    (void)host;
    int32_t a = native::ArgInt(args, 0);
    int32_t b = native::ArgInt(args, 1);
    native::ReturnInt(ret, a + b);
}

void sample_double_float(NativeHost* host, uint8_t* ret,
                         const uint8_t* args, int /*argc*/) {
    (void)host;
    float x = native::ArgFloat(args, 0);
    native::ReturnFloat(ret, x * 2.0f);
}

void sample_greet(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int /*argc*/) {
    std::string name = native::ArgString(host, args, 0);
    native::ReturnString(host, ret, std::string("hello ") + name);
}

NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "sample", "add", &sample_add);
    reg(registry, "sample", "doubleFloat", &sample_double_float);
    reg(registry, "sample", "greet", &sample_greet);
    return static_cast<int>(NLANG_HOST_ABI_VERSION);
}
