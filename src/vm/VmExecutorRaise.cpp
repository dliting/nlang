/*---
    VmExecutorRaise.cpp — exception raising and backtrace
    formatting: FormatBacktrace/FormatBacktraceFrame plus the
    RaiseNlangException/BuildNlangException raise family.
    Split out of VmExecutor.cpp (2026-11-12 source-size split,
    zero behavior change).
---*/
#include "VmExecutor.h"
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace nlang {

std::string VmExecutor::FormatBacktrace() const {
    std::string out;
    std::string moduleName = m_currModule ? m_currModule->name : "<module>";
    //UnwindFrames() is innermost-first (innermost's destructor ran first).
    //v0.8.5: each frame names ITS compiled translation unit, not the
    //current module (single-file builds only looked right by accident).
    for (const auto& f : UnwindFrames())
        out += "  at "
            + FormatBacktraceFrame(f.funcName, f.sourceFile, f.currentLine,
                                   moduleName)
            + "\n";
    return out;
}

//v0.8.5: see the header. std::string concatenation, not snprintf: paths
//on Windows exceed no buffer and need no truncation policy.
std::string FormatBacktraceFrame(const std::string& funcName,
    const std::string& file, uint16_t line,
    const std::string& fallbackModule) {
    //Legacy spelling: "<module>.n" — the fallback owns the suffix so
    //callers pass the bare module name.
    const std::string loc = !file.empty() ? file : fallbackModule + ".n";
    return funcName + " (" + loc + ":"
        + (line != 0 ? std::to_string(static_cast<unsigned>(line))
                     : std::string("?")) + ")";
}
//Phase 9d: build a fresh Exception instance of the requested subclass,
//populate message + backtrace, then throw NLangThrow to unwind to the
//nearest catch handler (or top-level if none).
[[noreturn]] void VmExecutor::RaiseNlangException(int16_t classIdx,
                                                   const std::string& msg) {
    //K8: the exception build self-locks (one Container section); the
    //guard stays for FireOnThrow — the throw-site checkpoint takes the
    //scheduler domain lock (same family as OpThrow/OpRethrow).
    GilEnsureGuard g(*this);
    //v0.13.6 I1: the mint+root span crosses the native-minter gate. A
    //raiser parked in a blocking native (or a host thread through the
    //bridge) builds the exception graph OUTSIDE the rendezvous
    //denominator, so an elected collection could sweep the fresh slots
    //between the mints and the root push — the same race the gate
    //closes for strings and arrays (see VmExecutor.h). The gate scope
    //must cover the whole graph (instance, message, backtrace): the
    //earlier handles only become durable at the single root push at
    //the end, so a collection slipping in between two mint calls with
    //the gate released would sweep them (review I2). The root push
    //itself registers only inside a blocking-native section; an
    //on-carrier raise keeps the bare handle, where the quiescence
    //contract already covers the span.
    int32_t heapIdx = 0;
    {
        NativeMintGate mintGate(*this);
        heapIdx = BuildNlangException(classIdx, msg);
        RootMintedNativeHandle(heapIdx);
    }
    //v0.13.6 I1 white-box seam: when armed, pause after the mint and let
    //real collections run against the raised exception before it reaches
    //a durable holder (the production window is the GIL gap on the
    //unwind path, so the pause drops the GIL through a handoff scope —
    //sleeping under the GIL would freeze the collectors this seam must
    //exercise and stall against an elected one mid-collection). After
    //the pause the seam scans the native-call root registry for the value-form
    //entry RootMintedNativeHandle pushes for heapIdx. Presence is the
    //rooting contract itself and is GC-timing-independent, which survival
    //checking is not: the threshold backoff decides whether any
    //collection fires inside the window at all, and a resident root can
    //keep unrooted exception objects alive through value aliasing
    //(v0.13.6 review I1 forensics). Production never arms this knob.
    const int gapMs = m_mintRootGapMs.load(std::memory_order_relaxed);
    if (gapMs > 0) {
        {
            GilHandoffScope gapWindow(*this);
            std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
        }
        bool rooted = false;
        std::lock_guard<std::mutex> rg(m_rootRegistryMutex);
        for (const NativeCallRoot& r : m_nativeCallRoots) {
            if (!r.cells && r.value == heapIdx) {
                rooted = true;
                break;
            }
        }
        m_lastRaiseMintRootedForTests = rooted;
    }
    //Debugger checkpoint at the throw site, before unwinding starts
    //(the full NLang stack is still alive).
    FireOnThrow();
    //v0.12.4 D8: route the report through ThrownMessageOf so VM-raised
    //failures print the same "<Type>: <message>" top line as user
    //throws (the old raw msg lost the type name).
    throw NLangThrow(heapIdx, ThrownMessageOf(heapIdx, msg.c_str()));
}

//K2 slice 4: the building half of RaiseNlangException, split out so
//NativeRaiseException can root the fresh object before throwing —
//between the throw and the blocking arm's catch the GIL is away (the
//unwinding callback guard releases it), and a GC on another carrier
//must not sweep the in-flight exception.
int32_t VmExecutor::BuildNlangException(int16_t classIdx,
                                         const std::string& msg) {
    //K8: single Container-domain lock (the backtrace List slot); allocations and mints are forward.
    DomainGuard g(m_domainLocks, DomainId::Container);
    if (classIdx < 0)
        throw std::runtime_error("NLang VM: exception class not registered");
    int32_t heapIdx = AllocClassOnHeap(static_cast<uint16_t>(classIdx));

    //cell[1] = message. Mint a string object, store its handle.
    //Do NOT hold a reference to m_structHeap[heapIdx] across the List
    //allocation below — AllocClassOnHeap may push_back to m_structHeap and
    //invalidate the reference (vector resize).
    int32_t msgHandle = MintNewString(msg);
    m_structHeap[static_cast<size_t>(heapIdx)][1] = msgHandle;

    //cell[5] = cause (v0.12.4 D7): VM-raised exceptions wrap nothing,
    //so the field is explicitly null — never rely on allocation zeroing.
    m_structHeap[static_cast<size_t>(heapIdx)][5] = 0;

    //cell[3] = backtrace (field 1 under the uniform 2-cell stride):
    //one boxed frame string per active call entry, innermost-first
    //(MintBacktraceList lives with the exception-formatting family in
    //VmExecutorOpsControl.cpp).
    int32_t listHeapIdx = MintBacktraceList();
    //Now write listHeapIdx to cell[3] of the Exception. Safe because no
    //further allocations happen before the throw.
    if (listHeapIdx > 0)
        m_structHeap[static_cast<size_t>(heapIdx)][3] = listHeapIdx;
    return heapIdx;
}
} // namespace nlang
