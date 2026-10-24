/*---
TensorKernels.h — internal (in-DLL) interface of the tensor native
module: shared payload helpers and the entry-point declarations the
registration unit binds. Not installed; only the module's own .cpp
files include it.
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

#include <cstdint>

namespace tensor {

//The view's data pointer aliases the record's raw cells; each element
//type reads/writes its own carrier width. elemCellBytes is asserted by
//the callers' kind checks, not re-checked here.
template <typename T>
T* CellsOf(const NativeArrayView& v) {
    return static_cast<T*>(v.data);
}

//--- creation entries (tensor_native.cpp) --------------------------------------

void ZerosI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ZerosL(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ZerosF(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ZerosD(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void OnesI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void OnesL(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void OnesF(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void OnesD(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullL(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullF(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullD(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ArangeI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ArangeL(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ArangeF(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ArangeD(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);

//--- elementwise binary entries (tensor_binary.cpp) -----------------------------

void TensorAdd(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorSub(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorMul(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorDiv(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);

} // namespace tensor
