/*---
backend_stream.cpp — the v0.14.9 multi-stream orchestration dispatch of
the `tensor` native module: the registry-level routing over the active
backend's stream pool (the cuda plugin's pool functions; a no-op on
cpu/mirror — the language program stays identical across backends).
Split from backend_registry.cpp for the source-size guard (the
registry file caps at 500 lines).

Device ABI v2: pushed handles encode their device (the plugin's pool
handles are `device * kNativeMaxStreams + slot`), so the registry-side
push stack keeps the handles themselves — CurrentScopeDevice decodes
the top, and the creation mints land on the current stream's device.
---*/
#include "backend_registry.h"
#include "../tensor_cuda/backend_descriptor.h"

namespace tensor {

using nlang::native::Raise;

//The push/pop stack balance: the language's stream discipline is the
//orchestration contract — an unbalanced Pop would silently misroute
//subsequent ops to the wrong stream, so the rejection is part of the
//contract (per-process: the streams are process state, matching the
//backend's own lifetime). The stack records the pushed handles (the
//current-scope device reads through them); the depth counter doubles
//as the stack size — the PRNG window's StreamPushDepth() contract is
//spelled exactly as before. The nesting cap is the descriptor's
//shared bound (the plugin's push stack is sized by the same number).
static int32_t g_streamStack[kNativeMaxStreamPushDepth];
static int32_t g_streamPushDepth = 0;

//The push depth: 0 on the main stream, >0 inside a Push/Pop scope (a
//chunk stream). The PRNG window dispatch reads this to decide whether
//a uniform draw takes a chunk window (snapshot) or rides the live
//engine (main stream).
int32_t StreamPushDepth() {
    return g_streamPushDepth;
}

//The device of the current scope: the top handle's encoded device; 0
//outside any Push (handle 0 — the implicit main stream — decodes to
//device 0 anyway, so the empty stack and the main stream agree).
int32_t CurrentScopeDevice() {
    if (g_streamPushDepth == 0)
        return 0;
    return DecodeStreamDevice(g_streamStack[g_streamPushDepth - 1]);
}

int32_t BackendStreamCreate(int32_t device) {
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (backend.streamCreate)
        return backend.streamCreate(device);
    return 0;   //host/mirror: handle 0 = the implicit main "stream"
}

void BackendStreamDestroy(int32_t handle) {
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (backend.streamDestroy)
        backend.streamDestroy(handle);
}

void BackendStreamPush(NativeHost* host, int32_t handle) {
    //The nesting cap guards the fixed stack: real orchestration nests
    //one level per concurrent chunk, far below it, so a breach names
    //the fault instead of corrupting the stack.
    if (g_streamPushDepth >= kNativeMaxStreamPushDepth)
        Raise(host, NEXC_Base,
              "tensor.streamPush: stream nesting deeper than "
              + std::to_string(kNativeMaxStreamPushDepth));
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (backend.streamPush)
        backend.streamPush(handle);
    //The balance is tracked even when the backend has no pool: the
    //language discipline (matched Push/Pop) is the contract, and the
    //program runs identically on cpu/mirror (no-op streams).
    g_streamStack[g_streamPushDepth] = handle;
    g_streamPushDepth += 1;
}

void BackendStreamPop(NativeHost* host) {
    if (g_streamPushDepth == 0)
        Raise(host, NEXC_Base,
              "tensor.streamPop: no matching streamPush");
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (backend.streamPop)
        backend.streamPop();
    g_streamPushDepth -= 1;
}

void BackendStreamJoin(int32_t handle) {
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (backend.streamJoin)
        backend.streamJoin(handle);
}

} // namespace tensor
