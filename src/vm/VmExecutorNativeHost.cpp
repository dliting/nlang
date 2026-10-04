/*---
VmExecutorNativeHost.cpp — VmExecutor's implementation of the public
NativeHost ABI: the function-table callbacks, lazy native-module loading
and the one-shot List<string> construction.

Native code (standard-library DLLs and third-party modules) accesses the VM
only through these callbacks; the mutable state (PRNG, string/list stores)
stays here, in the executor.
---*/
#include "VmExecutor.h"
#include "VmExecutorSer.h"
#include "NativeLibraryLoader.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace nlang {

namespace {
//A native side holds NativeHost* equal to &vmNativeHost.c (the first
//member); restore the wrapper.
VmNativeHost* Wrap(NativeHost* self) {
    return reinterpret_cast<VmNativeHost*>(self);
}
} // namespace

void VmExecutor::InitNativeHost(VmNativeHost& host) {
    host.executor = this;
    host.stringScratch.clear();
    host.lineScratch.clear();
    host.c.abiVersion = NLANG_HOST_ABI_VERSION;
    host.c.getString = &VmExecutor::NativeGetString;
    host.c.newString = &VmExecutor::NativeNewString;
    host.c.newListString = &VmExecutor::NativeNewListString;
    host.c.writeOutput = &VmExecutor::NativeWriteOutput;
    host.c.writeError = &VmExecutor::NativeWriteError;
    host.c.readLine = &VmExecutor::NativeReadLine;
    host.c.raiseException = &VmExecutor::NativeRaiseException;
    host.c.nextRandom = &VmExecutor::NativeNextRandom;
    host.c.seedRandom = &VmExecutor::NativeSeedRandom;
}

//--- NativeHost callbacks ---------------------------------------------------

const char* VmExecutor::NativeGetString(NativeHost* self, int32_t handle) {
    VmNativeHost* h = Wrap(self);
    h->stringScratch = h->executor->StrValCopy(handle);
    return h->stringScratch.c_str();
}

int32_t VmExecutor::NativeNewString(NativeHost* self, const char* utf8) {
    return Wrap(self)->executor->MintNewString(utf8 ? utf8 : "");
}

int32_t VmExecutor::NativeNewListString(NativeHost* self,
                                        const char* const* items,
                                        int count) {
    return Wrap(self)->executor->BuildListString(items, count);
}

void VmExecutor::NativeWriteOutput(NativeHost* self, const char* text) {
    VmExecutor* e = Wrap(self)->executor;
    const char* out = text ? text : "";
    if (e->m_pHostIo) {
        e->m_pHostIo->OnOutput(out);
    } else {
        std::fwrite(out, 1, std::strlen(out), stdout);
        std::fflush(stdout);
    }
}

void VmExecutor::NativeWriteError(NativeHost* self, const char* text) {
    VmExecutor* e = Wrap(self)->executor;
    const char* out = text ? text : "";
    if (e->m_pHostIo) {
        //A debug session has one merged output view — diagnostics
        //interleave with stdout there by design.
        e->m_pHostIo->OnOutput(out);
    } else {
        std::fwrite(out, 1, std::strlen(out), stderr);
        std::fflush(stderr);
    }
}

const char* VmExecutor::NativeReadLine(NativeHost* self) {
    VmNativeHost* h = Wrap(self);
    VmExecutor* e = h->executor;
    //An installed host IS the input channel: it supplies whole lines
    //(blocking allowed — the program is parked here) and a host with no
    //input returns false, so readLine fails loudly instead of silently
    //consuming the embedder's stream (machine mode keeps stdin as its
    //protocol channel). No host at all keeps console getline (nvm/CLI).
    std::string line;
    if (e->m_pHostIo) {
        if (!e->m_pHostIo->ReadInputLine(line))
            e->RaiseNlangException(e->m_ioExcClassIdx,
                "io.readLine: stdin is not available in this session.");
    }
    else if (!std::getline(std::cin, line)) {
        //getline fails (and leaves line empty) at EOF with no chars
        //read, so an empty final line and EOF are indistinguishable —
        //documented semantics rather than a defect.
        line.clear();
    }
    //Strip a trailing '\r' from either source: console CRLF framing
    //and a host that passes "\r\n"-shaped lines both normalize here.
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    h->lineScratch = std::move(line);
    return h->lineScratch.c_str();
}

void VmExecutor::NativeRaiseException(NativeHost* self, int exceptionKind,
                                      const char* message) {
    VmExecutor* e = Wrap(self)->executor;
    int16_t classIdx = (exceptionKind == NEXC_IOException)
        ? e->m_ioExcClassIdx : e->m_exceptionClassIdx;
    e->RaiseNlangException(classIdx, message ? message : "");
}

uint32_t VmExecutor::NativeNextRandom(NativeHost* self) {
    return Wrap(self)->executor->m_rng();
}

void VmExecutor::NativeSeedRandom(NativeHost* self, int32_t seed) {
    Wrap(self)->executor->m_rng.seed(static_cast<uint32_t>(seed));
}

//--- List<string> construction ---------------------------------------------

int32_t VmExecutor::BuildListString(const char* const* items, int count) {
    if (m_listClassIdx < 0)
        throw std::runtime_error("NLang VM: List class not registered");
    int32_t listHeapIdx = AllocClassOnHeap(
        static_cast<uint16_t>(m_listClassIdx));
    int32_t listHandle = AllocListHandle();
    m_structHeap[static_cast<size_t>(listHeapIdx)]
        [kListHandleFieldOffset] = listHandle;
    auto& dst = m_listStore[listHandle - 1].elements;
    if (count > 0)
        dst.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const char* text = (items && items[i]) ? items[i] : "";
        dst.push_back(AllocBoxedValue(RTK_String, MintNewString(text)));
    }
    return listHeapIdx;
}

//--- Native module loading --------------------------------------------------

NativeLibraryLoader& VmExecutor::EnsureNativeLoader() {
    if (!m_upNativeLoader) {
        m_upNativeLoader = std::make_unique<NativeLibraryLoader>();
        //Default: native modules ship beside the executable.
        m_upNativeLoader->AddSearchDir(NativeLibraryLoader::ExecutableDir());
    }
    return *m_upNativeLoader;
}

void VmExecutor::AddNativeSearchDir(std::string dir) {
    EnsureNativeLoader().AddSearchDir(std::move(dir));
}

void VmExecutor::EnsureNativeAvailable(const std::string& name) {
    if (m_natives.find(name) != m_natives.end())
        return;
    //A qualified "ns.name" is satisfied by a loadable module; a bare name
    //is a host-registered native and is not resolved through a DLL.
    size_t dot = name.find('.');
    if (dot == std::string::npos)
        return;
    std::string ns = name.substr(0, dot);
    NativeLibraryLoader& loader = EnsureNativeLoader();
    loader.EnsureLoaded(ns,
        [this](const std::string& modNs, const std::string& fnName,
               NativeFn fn) {
            m_natives[modNs + "." + fnName] = fn;
        });
}

} // namespace nlang
