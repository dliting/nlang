#pragma once

// VmNativeHost — the VmExecutor's binding of the public NativeHost ABI.
// The C ABI table is the first member so a NativeHost* the native side
// holds (equal to &wrapper.c) can be restored to VmNativeHost* by the
// static callbacks (standard-layout; first member shares the address).
//
// This header is private to src/vm. It only forward-declares VmExecutor to
// avoid a cycle with VmExecutor.h.

#include "nlang/vm/NativeHost.h"

#include <string>

namespace nlang {

class VmExecutor;

struct VmNativeHost {
    NativeHost c;                 // public C ABI function table (first)
    VmExecutor* executor = nullptr;
    std::string stringScratch;    // backs getString returned pointers
    std::string lineScratch;      // backs readLine/readToken pointers
};

} // namespace nlang
