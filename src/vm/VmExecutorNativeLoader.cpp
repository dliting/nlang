/*---
    VmExecutorNativeLoader.cpp — lazy native-module loading
    and the one-shot List<string> construction behind the
    native-callback surface. Split out of
    VmExecutorNativeHost.cpp (2026-11-12 source-size split,
    zero behavior change).
---*/
#include "VmExecutor.h"
#include "NativeLibraryLoader.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace nlang {

//--- List<string> construction ---------------------------------------------

int32_t VmExecutor::BuildListString(const char* const* items,
                                    const int64_t* lens, int count) {
    DomainGuard containerGuard(m_domainLocks, DomainId::Container);
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
        //v13: lens[i] bytes are the item's content (a NUL byte inside it
        //is content); a null item or a non-positive length is "".
        std::string content;
        if (items && items[i] && lens && lens[i] > 0)
            content.assign(items[i], static_cast<size_t>(lens[i]));
        dst.push_back(
            AllocBoxedValue(RTK_String, CreateNewString(std::move(content))));
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
    //The module init runs once per executor (each executor owns its
    //loader) with THIS executor's host — the device hooks land per
    //executor (the plan's L5 ruling).
    VmNativeHost initHost;
    InitNativeHost(initHost);
    loader.EnsureLoaded(ns,
        [this](const std::string& modNs, const std::string& fnName,
               NativeFn fn, bool blocking) {
            m_natives[modNs + "." + fnName] = NativeEntry{fn, blocking};
        },
        &initHost.c);
}
} // namespace nlang
