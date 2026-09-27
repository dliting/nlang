// Bad-ABI fixture: reports an incompatible ABI version so the loader must
// reject it.

#include "nlang/vm/NativeHost.h"

NLANG_DEFINE_NATIVE_INIT {
    (void)registry;
    (void)reg;
    return 999999;
}
