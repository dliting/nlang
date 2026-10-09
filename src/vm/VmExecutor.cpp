#include "VmExecutor.h"
#include "NativeLibraryLoader.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

VmExecutor::~VmExecutor() = default;

//2026-09-26 maintainability split: Execute's per-run phases. Bodies are
//verbatim extracts; Execute keeps the original operation order.
void VmExecutor::ResetPerRunState(const CompiledModule& module) {
    m_recurseDepth = 0;
    m_unwindFrames.clear();
    m_lastBacktrace.clear();
    m_byteStreams.clear();
    m_byteStreamFreeList.clear();
    m_fileStreams.clear();
    m_fileStreamFreeList.clear();
    //Phase 11 Q9: fresh entropy per run; math.srand replaces it on demand.
    m_rng.seed(std::random_device{}());
    //Phase 8e-3: reset List<T> side table and cache the class index.
    m_listStore.clear();
    m_listFreeList.clear();
    int listIdx = module.FindClass(kBuiltinListTypeName);
    m_listClassIdx = (listIdx >= 0) ? static_cast<int16_t>(listIdx) : -1;

    //Phase 8e-4: reset Dict<K,V> side table and cache the class index.
    m_dictStore.clear();
    m_dictFreeList.clear();
    int dictIdx = module.FindClass(kBuiltinDictTypeName);
    m_dictClassIdx = (dictIdx >= 0) ? static_cast<int16_t>(dictIdx) : -1;

    //Phase 9d: cache Exception hierarchy class indices. These are required
    //by RaiseNlangException to allocate the right subclass instance.
    //All FindClass calls in this function use bare names deliberately:
    //the builtin List/Dict/Exception family carries no owner tag, so its
    //table keys stay bare even after phase 5 qualified user keys.
    auto cacheExcClass = [&](const char* name, int16_t* out) {
        int idx = module.FindClass(name);
        *out = (idx >= 0) ? static_cast<int16_t>(idx) : -1;
    };
    cacheExcClass("Exception",                &m_exceptionClassIdx);
    cacheExcClass("NullPointerException",     &m_nullPtrExcClassIdx);
    cacheExcClass("DivByZeroException",       &m_divZeroExcClassIdx);
    cacheExcClass("IndexOutOfBoundsException", &m_oobExcClassIdx);
    cacheExcClass("AssertionException",       &m_assertExcClassIdx);
    cacheExcClass("IOException",              &m_ioExcClassIdx);
}

//String objectization: eagerly materialize every module string constant
//as an immortal Flat object (JVM constant-pool semantics — constants
//live as long as the execution). Value consumers resolve constant
//indexes through m_constStrCache and never see raw indexes.
void VmExecutor::InitStringStore(const CompiledModule& module) {
    m_stringObjs.clear();
    m_strMarkBits.clear();
    m_strFreeList.clear();
    m_shortStrTable.clear();
    m_stringObjs.emplace_back();   //sentinel slot 0 (handle 0 = null)
    m_stringObjs[0].form = static_cast<StrObj::Form>(kStrFormDead);
    m_strMarkBits.push_back(false);
    m_constStrCache.clear();
    m_constStrCache.reserve(module.stringConstants.size());
    for (const std::string& s : module.stringConstants)
        m_constStrCache.push_back(MintConstantString(s));
    m_emptyStrHandle = MintConstantString("");
}

//Initialize struct heap with sentinel at index 0, plus GC state.
void VmExecutor::InitStructHeap() {
    m_structHeap.clear();
    m_structHeap.emplace_back();  //empty slot at index 0

    m_slotKinds.clear();
    m_slotKinds.push_back(0);    //sentinel
    m_slotStructIdx.clear();
    m_slotStructIdx.push_back(0);
    m_freeList.clear();
    m_gcPending = false;
    m_callStack.clear();
}

void VmExecutor::InitializeForRun(const CompiledModule& module) {
    //Reset per-run state up front: if the executor is reused (e.g. a
    //future REPL), an early throw below must not expose stale frames
    //or backtrace from a previous run.
    m_currModule = &module;
    ResetPerRunState(module);
    InitStringStore(module);
    InitStructHeap();
}

int VmExecutor::Execute(const CompiledModule& module) {
    InitializeForRun(module);

    //Only the root module's entryPoint is meaningful; imported modules
    //carry -1 (and merged function-table indices would shift anyway), so
    //there is deliberately no by-name fallback here. (Order note: this
    //check used to sit between the reset and the store init; after the
    //extraction it runs after the full init — not externally observable,
    //as the throw happens before any observable side effect.)
    int mainIdx = module.entryPoint;
    if (mainIdx < 0
        || mainIdx >= static_cast<int32_t>(module.functions.size()))
        throw std::runtime_error("NLang VM: module has no entry point");

    const CompiledFunction& mainFunc = module.functions[mainIdx];
    std::vector<uint8_t> locals(mainFunc.localsSize, 0);
    //pResult contract is a full uniform frame cell (kFrameSlotBytes): the
    //callee's final OP_VarLocal return copy writes 8 bytes, so a narrow
    //host-side int would be overflowed (ASan-confirmed stack corruption).
    //The 4-byte main return value sits in the low half.
    alignas(int64_t) uint8_t resultCell[kFrameSlotBytes] = {0};
    try {
        //Phase 9f: a pathological `native int main();` still dispatches
        //through the host table — executing the declaration's empty
        //bytecode would silently return 0.
        if (mainFunc.isNative)
            CallNative(mainFunc, 0, locals.data(), resultCell);
        else
            ExecuteFunction(mainFunc, resultCell, locals.data());
    } catch (const std::exception&) {
        m_lastBacktrace = FormatBacktrace();
        throw;
    }
    int32_t result;
    std::memcpy(&result, resultCell, sizeof(result));
    return result;
}

std::string VmExecutor::FormatBacktrace() const {
    std::string out;
    std::string moduleName = m_currModule ? m_currModule->name : "<module>";
    char buf[256];
    //m_unwindFrames is innermost-first (innermost's destructor ran first).
    for (const auto& f : m_unwindFrames) {
        if (f.currentLine != 0) {
            std::snprintf(buf, sizeof(buf), "  at %s (%s.n:%u)\n",
                f.funcName.c_str(), moduleName.c_str(),
                static_cast<unsigned>(f.currentLine));
        } else {
            std::snprintf(buf, sizeof(buf), "  at %s (%s.n:?)\n",
                f.funcName.c_str(), moduleName.c_str());
        }
        out += buf;
    }
    return out;
}
//Phase 9d: build a fresh Exception instance of the requested subclass,
//populate message + backtrace, then throw NLangThrow to unwind to the
//nearest catch handler (or top-level if none).
[[noreturn]] void VmExecutor::RaiseNlangException(int16_t classIdx,
                                                   const std::string& msg) {
    if (classIdx < 0)
        throw std::runtime_error("NLang VM: exception class not registered");
    int32_t heapIdx = AllocClassOnHeap(static_cast<uint16_t>(classIdx));

    //cell[1] = message. Mint a string object, store its handle.
    //Do NOT hold a reference to m_structHeap[heapIdx] across the List
    //allocation below — AllocClassOnHeap may push_back to m_structHeap and
    //invalidate the reference (vector resize).
    int32_t msgHandle = MintNewString(msg);
    m_structHeap[static_cast<size_t>(heapIdx)][1] = msgHandle;

    //cell[3] = backtrace (field 1 under the uniform 2-cell stride).
    //Allocate a List<string> and push one entry per active call frame,
    //innermost-first.
    int32_t listHeapIdx = -1;
    if (m_listClassIdx >= 0) {
        listHeapIdx = AllocClassOnHeap(static_cast<uint16_t>(m_listClassIdx));
        //Initialize List.__handle by allocating a fresh side-table slot.
        m_listStore.emplace_back();  //new empty ListSlot
        int32_t handle = static_cast<int32_t>(m_listStore.size());
        //__handle is the first field of List (slot[1]).
        m_structHeap[static_cast<size_t>(listHeapIdx)][1] = handle;
        //Populate the backtrace: walk m_callStack innermost-first.
        std::string moduleName = m_currModule ? m_currModule->name : "<module>";
        for (auto it = m_callStack.rbegin(); it != m_callStack.rend(); ++it) {
            char buf[256];
            if (it->currentLine != 0) {
                std::snprintf(buf, sizeof(buf), "%s (%s.n:%u)",
                    it->func ? it->func->name.c_str() : "<unknown>",
                    moduleName.c_str(),
                    static_cast<unsigned>(it->currentLine));
            } else {
                std::snprintf(buf, sizeof(buf), "%s (%s.n:?)",
                    it->func ? it->func->name.c_str() : "<unknown>",
                    moduleName.c_str());
            }
            //Frame text as before ("func (file.n:line)"); box the string so
            //the List element is a real RTK_Boxed record — the old raw pool
            //index made e.backtrace.get(i) raise "unbox on null/invalid
            //reference".
            int32_t frameHandle = MintNewString(buf);
            int32_t boxed = AllocBoxedValue(RTK_String, frameHandle);
            m_listStore[static_cast<size_t>(handle) - 1].elements.push_back(boxed);
        }
    }
    //Now write listHeapIdx to cell[3] of the Exception. Safe because no
    //further allocations happen before the throw.
    if (listHeapIdx > 0)
        m_structHeap[static_cast<size_t>(heapIdx)][3] = listHeapIdx;
    //Debugger checkpoint at the throw site, before unwinding starts
    //(the full NLang stack is still alive).
    FireOnThrow();
    throw NLangThrow(heapIdx, msg);
}

}
