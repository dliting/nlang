/*---
backend_stream.cpp — the v0.14.9 multi-stream orchestration dispatch of
the `tensor` native module: the registry-level routing over the active
backend's stream pool (the cuda plugin's pool functions; a no-op on
cpu/mirror — the language program stays identical across backends).
Split from backend_registry.cpp for the source-size guard (the
registry file caps at 500 lines).
---*/
#include "backend_registry.h"

namespace tensor {

using nlang::native::Raise;

//The push/pop stack balance: the language's stream discipline is the
//orchestration contract — an unbalanced Pop would silently misroute
//subsequent ops to the wrong stream, so the rejection is part of the
//contract (per-process: the streams are process state, matching the
//backend's own lifetime).
static int32_t g_streamPushDepth = 0;

//The push depth: 0 on the main stream, >0 inside a Push/Pop scope (a
//chunk stream). The PRNG window dispatch reads this to decide whether
//a uniform draw takes a chunk window (snapshot) or rides the live
//engine (main stream).
int32_t StreamPushDepth() {
    return g_streamPushDepth;
}

int32_t BackendStreamCreate() {
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (backend.streamCreate)
        return backend.streamCreate();
    return 0;   //host/mirror: handle 0 = the implicit main "stream"
}

void BackendStreamDestroy(int32_t handle) {
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (backend.streamDestroy)
        backend.streamDestroy(handle);
}

void BackendStreamPush(int32_t handle) {
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (backend.streamPush)
        backend.streamPush(handle);
    //The balance is tracked even when the backend has no pool: the
    //language discipline (matched Push/Pop) is the contract, and the
    //program runs identically on cpu/mirror (no-op streams).
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
