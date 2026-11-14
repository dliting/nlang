/*---
    tensor_ops_cpu.cpp — the CPU backend's TensorOps table + the whole-op
    registry (T1.1 skeleton).

    P1 state: the slots are filled family by family as the dispatchers
    migrate (T1.2a-e). A slot that has not migrated yet keeps the
    kNotYetMigrated stub — routing to it is a wiring bug, not a runtime
    path, so it asserts loudly. Once a family migrates, its stub is
    replaced by the real implementation calling the existing
    Elementwise.h drivers + KernelOps (the SIMD stack, untouched).

    The registry: the cpu table registers at static init;
    ActiveTensorOps() resolves NLANG_TENSOR_BACKEND (unset/unknown →
    cpu). RegisterTensorOps is the hook for out-of-TU backends (the P2
    mirror registration, the cuda plugin).
---*/
#include "TensorOps.h"

#include <cstring>
#include <cstdlib>
#include <string>

namespace tensor {

namespace {

//--- the cpu table: slots fill in as each family migrates (T1.2a-e).
//Unmigrated slots stay nullptr — the dispatcher still runs the host
//path directly until its family's slot is real, so a nullptr slot is
//never invoked mid-migration (migration order: implement slot, THEN
//switch the dispatcher to route through it).
TensorOps MakeCpuTable() {
    TensorOps t;
    std::memset(&t, 0, sizeof(t));
    return t;
}

const TensorOps kCpuTable = MakeCpuTable();

//Registered backend tables by name; the cpu table is always present.
struct BackendEntry {
    const char* name;
    const TensorOps* ops;
    BackendEntry* next;
};

BackendEntry* g_backends = nullptr;
const TensorOps* g_active = nullptr;   //resolved once, then cached

void RegisterBuiltinBackends() {
    static BackendEntry cpuEntry = {"cpu", &kCpuTable, nullptr};
    cpuEntry.next = g_backends;
    g_backends = &cpuEntry;
}

} //namespace

void RegisterTensorOps(const char* name, const TensorOps* ops) {
    if (g_backends == nullptr)
        RegisterBuiltinBackends();
    auto* entry = new BackendEntry{name, ops, g_backends};
    g_backends = entry;
    g_active = nullptr;   //re-resolve on next use
}

const TensorOps& ActiveTensorOps() {
    if (g_backends == nullptr)
        RegisterBuiltinBackends();
    if (g_active == nullptr) {
        const char* requested = std::getenv("NLANG_TENSOR_BACKEND");
        const TensorOps* found =
            (requested == nullptr) ? &kCpuTable : nullptr;
        for (BackendEntry* e = g_backends; e != nullptr && found == nullptr;
             e = e->next) {
            if (requested != nullptr && std::strcmp(e->name, requested) == 0)
                found = e->ops;
        }
        //Unknown/unset names resolve to the built-in cpu backend (the
        //KernelOps-era selector contract; device plugins register under
        //their own names and load their tables in T3.8).
        if (found == nullptr)
            found = &kCpuTable;
        g_active = found;
    }
    return *g_active;
}

} // namespace tensor
