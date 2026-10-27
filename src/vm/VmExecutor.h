#pragma once
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/NativeHost.h"
#include "BytecodeReader.h"
#include "IDebugHooks.h"
#include "IHostFunctions.h"
#include "IHostIo.h"
#include "TokenView.h"
#include "VmExecutorNativeHost.h"
#include "VmClassLayout.h"
#include "NativeLibraryLoader.h"
#include "CallFrame.h"
#include "ChunkedArray.h"
#include "ShardFreeList.h"
#include "VmDomainLocks.h"
#include "VmScheduler.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <fstream>
#include <mutex>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nlang {

//Phase 9d: unified C++ exception type for NLang-level throws. Carries the
//heap idx of the Exception instance (slot[0] = classIdx distinguishes the
//specific Exception subclass). Inherits std::runtime_error so the top-level
//`catch (const std::exception&)` in nvm/ncc main catches unhandled throws
//(otherwise std::terminate fires and the exit code is wrong).
struct NLangThrow : public std::runtime_error {
    int32_t heapIdx;
    NLangThrow(int32_t h, std::string msg)
        : std::runtime_error(msg), heapIdx(h) {}
};

//K8: deferred-raise carrier for code inside a domain-guarded section.
//RaiseNlangException builds an exception object (a Container acquisition)
//and runs FireOnThrow (the scheduler lock) — both reverse acquisitions
//under any domain. The guarded site throws this instead; the section
//boundary converts it into the real raise.
struct DeferredNlangRaise {
    int16_t classIdx;
    std::string message;
};

//v0.8.5: single source for one backtrace frame's text, shared by
//FormatBacktrace and RaiseNlangException's Exception.backtrace
//build (both used to fabricate "<module>.n" for every frame).
//file is the frame function's compiled translation unit (absolute path);
//empty falls back to the legacy "<module>.n" spelling (theoretical:
//synthetic stubs). line 0 prints "?" (the existing convention).
std::string FormatBacktraceFrame(const std::string& funcName,
    const std::string& file, uint16_t line,
    const std::string& fallbackModule);

//Phase 13: slot[2] of a function-handle record — selects the dispatch
//strategy in OP_CallDelegate. Slot contents differ by form: [0] holds a
//functions[] index for static handles, a string-constant index (method
//name) for virtual-dispatch handles.
enum FuncHandleForm : int32_t {
    kFuncFormStatic = 0,
    kFuncFormVirtual = 1,
};

class VmExecutor : public IVmDebugView {
public:
    VmExecutor() = default;
    ~VmExecutor();

    int Execute(const CompiledModule& module);

    //ndb: install a debugger front end. null (default) = previous
    //behavior; checkpoints then cost one null test per statement.
    void SetDebugHooks(IDebugHooks* hooks) { m_pDebugHooks = hooks; }

    //Machine mode / IDE front ends: install a host I/O sink. null
    //(default) = the executor keeps its stdout/stdin behavior.
    void SetHostIo(IHostIo* io) { m_pHostIo = io; }

    //Extension point ⑤ (spec 2026-10-08 §8): host-function dispatch
    //hook. CallNative asks it first; null (default) = unchanged
    //behavior (m_natives, then the lazy DLL path).
    void SetHostFunctions(IHostFunctions* hostFunctions) {
        m_pHostFunctions = hostFunctions;
    }

    //Extension point ① (spec 2026-10-08 §8): one-time run-state
    //initialization for a linked module — the Execute() entry sequence
    //extracted. Execute() still calls this per run (behavior unchanged);
    //embedding calls it once at load() so the heap and host proxies
    //survive across run()/call().
    void InitializeForRun(const CompiledModule& module);

    //K1 Slice 10: embedding run() entry — execute a loaded module's
    //function as the main context through the scheduler carrier WITHOUT
    //re-initializing per-run state (InitializeForRun ran at load();
    //re-running it would clear host-held values and GC roots created
    //between load() and run()). Suspensions are serviced like any
    //Execute() run: sleep wakes on its deadline, spawned contexts get
    //scheduled, and a main that parks with nothing left to run surfaces
    //as the catchable deadlock error. resultCell: 8 bytes out.
    void ExecuteLoadedEntry(uint16_t entryIdx, uint8_t* resultCell);

    //0.10.20: run-end report for spawned contexts whose failure no joiner
    //ever observed — the containment model used to swallow those
    //silently. Called on the Execute thread after the carriers have
    //joined (registry quiesced); one stderr line per unobserved failure.
    void ReportUnobservedSpawnedFailures();

    //Extension point ② (spec §8): invoke a free function without a
    //compiler-generated call site. argCells: argc marshalled 8-byte
    //cells (argc <= callee.paramCount); formals beyond argc fill from
    //callee.defaultValues — a non-constant (Unfoldable) default throws.
    //resultCell: 8 bytes out, caller reads per the declared return kind.
    //Throws std::runtime_error on unknown name; NLangThrow propagates
    //with the backtrace captured.
    void CallByName(const std::string& qualifiedName,
                    const uint8_t* argCells, uint32_t argc,
                    uint8_t* resultCell);
    //Index form: the adapter resolves overloads itself and passes the
    //functions[] index (CallByName is a FindFunction + this).
    void CallFunctionByIdx(uint16_t funcIndex, const uint8_t* argCells,
                           uint32_t argc, uint8_t* resultCell);

    //Host value bridge (extension point ② surface): the minimal
    //minting/allocation/typed-access primitives the embedding adapter
    //needs. Thin public wrappers over existing internal machinery —
    //no new semantics.
    //K7: single-embedding-thread constraint — these wrappers do not
    //self-guard the stores they touch; a second embedding thread
    //would need each of them guarded first.
    int32_t MintHostString(const std::string& content);
    int32_t BoxHostScalar(uint8_t typeTag, int64_t bits);
    int32_t AllocHostList();
    int32_t AllocHostDict();
    void HostListPushBack(int32_t listHeapIdx, int32_t elemHeapIdx);
    void HostDictUpsert(int32_t dictHeapIdx, int32_t keyHeapIdx,
                        int32_t valHeapIdx);
    bool HostKeysEqual(int32_t aHeapIdx, int32_t bHeapIdx);
    uint32_t HostArrayLength(int32_t heapIdx) const;
    void HostArrayGetCell(int32_t heapIdx, uint32_t index,
                          uint8_t outCell[8]) const;
    void HostArraySetCell(int32_t heapIdx, uint32_t index,
                          const uint8_t cell[8]);
    uint32_t HostListSize(int32_t heapIdx) const;
    int32_t HostListGet(int32_t heapIdx, uint32_t index) const;
    bool HostDictEntryGet(int32_t heapIdx, uint32_t index,
                          int32_t& keyOut, int32_t& valOut) const;
    uint32_t HostDictSize(int32_t heapIdx) const;
    bool HostBoxedRead(int32_t boxHeapIdx, uint8_t& tagOut,
                       int64_t& bitsOut) const;
    //Object/struct field cell access per the DECLARED field kind. The
    //descriptor is resolved from the heap slot's runtime kind — class
    //instances: slot[0]=classIdx into classes[]; struct instances:
    //m_slotStructIdx into structs[] (a separate table). Stride is
    //uniform 2 cells/field: class field i at cell 1+2i, struct field i
    //at 2i (pinned: VmExecutorDebug.cpp FormatDebugClassInstance/
    //FormatDebugStructInstance/FormatDebugField). 8-byte kinds
    //(RTK_Long/RTK_ULong/RTK_Double) span the cell pair [base,base+1].
    //Get returns the declared RTK so the caller decodes per declaration.
    uint8_t HostFieldCellGet(int32_t heapIdx, uint16_t fieldIndex,
                             uint8_t outCell[8]) const;
    void HostFieldCellSet(int32_t heapIdx, uint16_t fieldIndex,
                          const uint8_t cell[8]);
    bool HostFieldNameToIndex(int32_t heapIdx, const std::string& fieldName,
                              uint16_t& indexOut) const;
    //Class instances only: the declaring CompiledClass's name (slot[0]
    //class idx → classes[]). Used by the adapter to translate thrown
    //exceptions ("Exception", "IOException", ...).
    std::string HostClassName(int32_t heapIdx) const;

    //Proxy-surface mutators and probes (Task 9; impl in
    //VmExecutorHostProxyBridge.cpp). Same contract as the accessors
    //above: thin wrappers, std::runtime_error on bad indices/handles.
    void HostListSet(int32_t listHeapIdx, uint32_t index,
                     int32_t elemHeapIdx);
    void HostListRemoveAt(int32_t listHeapIdx, uint32_t index);
    void HostListClear(int32_t listHeapIdx);
    void HostDictRemove(int32_t dictHeapIdx, int32_t keyHeapIdx);
    void HostDictClear(int32_t dictHeapIdx);
    //Array element kind from the array-type table ([1]=arrayTypeIdx).
    uint8_t HostArrayElemKind(int32_t heapIdx) const;
    //Slot runtime kind (m_slotKinds) — boxed/class discrimination for
    //element decode (class instances are 3+ cells, size alone lies).
    uint8_t HostSlotKind(int32_t heapIdx) const;
    //K8: rethrow target for a host-held script exception. The instance
    //may have been collected between the escape into host code and a
    //later rethrow (host memory is not scanned). Probe: the recorded
    //index still holds the same class and message → same instance;
    //otherwise mint a fresh instance from the saved fields. The remint
    //is observationally equivalent (same class, same message; the fresh
    //backtrace reflects the rethrow site, and any pre-death script
    //mutation of the original was lost by the collection either way).
    //The target is rooted for the unwind — NativeRaiseException's
    //build-root-throw choreography.
    int32_t ExceptionRethrowTarget(const std::string& exceptionClass,
                                   const std::string& message,
                                   int32_t maybeLiveHeapIdx);

    //Testing knobs (white-box GC pressure): clamp both thresholds so any
    //untraced handle turns stale almost immediately, and observe the live
    //string-object population for bounded-memory assertions. The
    //post-sweep backoff still applies (pre-K6 stress semantics): the
    //tiny clamp keeps the store small, so the cadence stays high —
    //hundreds of collections per churn run — without forcing every
    //safepoint of a growing live set (quadratic; the deep-chain e2e
    //pins the timing).
    //K8: the clamp also shrinks the allocation growth quantum to one
    //slot — block batching would pad the store past the clamp and the
    //sweep's high-water floor would absorb the padding (first collection
    //starves every later one in a small run).
    void SetGcStressThresholds(size_t records) {
        m_gcThreshold = records;
        m_strGcThreshold = records;
        m_allocQuantum = 1;
    }
    //K6 D3: restore default thresholds. The test binaries run every
    //test function in one process — the stress clamp must be clearable
    //or it leaks into later tests.
    void ResetGcThresholds() {
        m_gcThreshold = GC_THRESHOLD_DEFAULT;
        m_strGcThreshold = kStrGcThresholdDefault;
        m_allocQuantum = DEFAULT_SHARD_BLOCK;
    }
    size_t LiveStringObjectCount() const;   //live slots, immortal included
    //K6 D7 white-box observability: collection count (per run; resets
    //with ResetPerRunState) and live heap slots (kinds != free).
    size_t GcCollectionCount() const { return m_gcCollections; }
    size_t LiveHeapSlotCount() const;
    //K7: this thread's own executing bit — registry-membership observability
    //(the white-box shape the K6 window count served: parked-in-native reads
    //false, a nested evaluation window reads true, quiescent reads false).
    bool GcExecutingSelf() const {
        return m_spScheduler->GcStop().ExecutingSelf();
    }
    //K7: per-run count of rendezvous parks — arrivals that blocked in
    //WaitForCompletion instead of collecting inline (single-carrier
    //degenerate runs read 0; real multiparty runs read > 0).
    int GcRendezvousParks() const {
        return m_gcRendezvousParks.load(std::memory_order_relaxed);
    }
    //K8 Task 6: per-run per-domain lock telemetry (acquisitions,
    //contended acquisitions, cumulative wait microseconds) — the
    //successor of the K7 store-guard counters, which died with the
    //domain split. Cleared by ResetPerRunState.
    DomainTelemetry DomainTelemetrySnapshot() const {
        return m_domainLocks.Snapshot();
    }
    //K8 Task 6 white-box seam: drive this executor's domain lock set
    //from test threads (deterministic contention for the telemetry
    //contract). Production code never calls this.
    DomainLockSet& DomainLocksForTests() { return m_domainLocks; }
    //K8 Task 5 white-box knob: make the NEXT collection throw once at
    //its entry — proves the collector's completion guard releases the
    //rendezvous waiters instead of stranding them on the epoch.
    void SetThrowOnceInCollectForTests() {
        m_gcThrowOnceForTests.store(true, std::memory_order_relaxed);
    }
    //K6 D11 white-box hooks (test_native_root_owner_removal): drive the
    //native-root ownership set through the same push/remove paths the
    //blocking arm uses. Production code never calls these.
    void TestPushNativeRoot(const uint8_t* cells, uint32_t cellCount,
                            int32_t value, uint64_t owner) {
        std::lock_guard<std::mutex> g(m_rootRegistryMutex);
        m_nativeCallRoots.push_back(
            NativeCallRoot{cells, cellCount, value, owner});
    }
    void TestRemoveNativeRoots(uint64_t owner) { RemoveNativeRoots(owner); }
    size_t TestNativeRootCount() const { return m_nativeCallRoots.size(); }
    //K2 slice 5 white-box knob: force the conservative both-domain band
    //scan even when a valid handle mask is present — the A/B baseline
    //for proving the precise scan keeps the same object graph alive.
    void SetConservativeBandForTests(bool conservative) {
        m_conservativeBandForTests = conservative;
    }
    //0.11.4 B white-box probes: corrupt a live array record so tests can
    //drive the native callbacks against a malformed record (the defensive
    //checks are otherwise unreachable — records are VM-built). The tests
    //pair these with a stack-local VmNativeHost built by InitNativeHost,
    //mirroring the production native-call wiring.
    void TruncateArrayRecordForTests(int32_t handle, size_t cells) {
        m_structHeap[static_cast<size_t>(handle)].resize(cells);
    }
    void OverwriteArrayRecordCellForTests(int32_t handle, size_t cell,
                                          int32_t value) {
        m_structHeap[static_cast<size_t>(handle)][cell] = value;
    }
    //Builds a production-shaped stack-local host (the native-call path
    //wires one per call). Public for the white-box defense tests only:
    //VmNativeHost itself is private to src/vm, so the method is
    //unreachable for outside users.
    void InitNativeHost(VmNativeHost& host);

    //Testing knobs (scheduler quota injection + observability, K1
    //decision 5): clamp the back-edge trip limit, then observe the
    //quota-fire/skip counters and the context registry after a run.
    //Single-context runs must fire-and-skip forever — preemption
    //requires another runnable context (or a carrier waiting on the
    //GIL — the K2 yield rule).
    void SetSchedulerTripForTests(int backedges) {
        m_schedTripLimit = backedges > 0 ? backedges : 1;
    }
    int SchedulerQuotaFires() const { return m_quotaFires; }
    int SchedulerPreemptSkips() const { return m_preemptSkips; }
    //K7 white-box probe: the current thread's evaluation slot must be
    //empty around a completed run — pins the frame-privatization routing
    //(carrier slot / fallback slot) without exposing the stack itself.
    bool EvalStackProbeEmpty() { return EvalStack().empty(); }
    //K2 slice 2 white-box observability: contexts dispatched on
    //auxiliary carriers, and the peak number of carriers contending
    //for the GIL — together they prove a configured pool genuinely
    //works (not a silently single-carrier no-op).
    int SchedulerAuxDispatchCount() const {
        return m_spScheduler->AuxDispatchCount();
    }
    int SchedulerGilWaitersPeak() const {
        return m_spScheduler->GilWaitersPeak();
    }
    //K5 batch 1 white-box observability: the GIL wait-time dimension —
    //cumulative nanoseconds waited and acquisitions that began while
    //another waiter was already queued. Per run (cleared by the
    //scheduler's Reset).
    uint64_t SchedulerGilWaitNanosTotal() const {
        return m_spScheduler->GilWaitNanosTotal();
    }
    uint64_t SchedulerGilContendedAcquires() const {
        return m_spScheduler->GilContendedAcquires();
    }
    //K4: carrier -> context currently held in its execution window (the
    //scheduler's GIL-guarded mirror; -1 = idle). White-box only — the
    //threads listing and tests consume it.
    int SchedulerHeldContextOfCarrier(int carrierId) const {
        return m_spScheduler->HeldContextOfCarrier(carrierId);
    }
    //GIL removal batch 1 (§7.2) white-box observability: is a native
    //name in the registry (host table or pre-warmed module)? Pins the
    //run-init prewarm invariant without forcing a call.
    bool IsNativeRegistered(const std::string& name) const {
        return m_natives.count(name) != 0;
    }

    //K2 slice 2 (plan ruling P1): carrier-thread count for the next run —
    //1 means the Execute thread alone (the library default: an embedding
    //host opts into parallel carriers explicitly); n>1 adds n-1 auxiliary
    //carriers, launched per run and joined at its end. Must be called
    //before Execute; takes effect on the next run.
    void SetCarrierThreadCount(uint32_t threads) {
        m_carrierThreadCount = threads > 0 ? threads : 1;
    }
    uint32_t CarrierThreadCount() const { return m_carrierThreadCount; }
    size_t SchedulerContextCount() const {
        return m_spScheduler->ContextCount();
    }
    const ExecutionContext* SchedulerContext(int id) const {
        return m_spScheduler->Find(id);
    }

    //IVmDebugView — definitions in VmExecutorDebug.cpp. Frame queries
    //address one context by id: the context in this carrier's execution
    //window reads the current thread's evaluation stack, every other context reads its parked
    //frames in the scheduler registry (K4: unified inspection face).
    size_t ContextCount() const override;
    DebugContextInfo ContextInfo(int contextId) const override;
    size_t FrameCount(int contextId) const override;
    DebugFrameInfo FrameInfo(int contextId, size_t depth) const override;
    std::vector<DebugLocalValue> FrameLocals(int contextId,
        size_t depth) const override;
    //K7 task 5: the freeze parking bracket (see IDebugHooks.h). The
    //frozen carrier leaves the GC rendezvous denominator, hands off any
    //held global lock, and takes a count on the dispatch gate; exit
    //mirrors it (the last one out opens the gate).
    void EnterFreezePark(GilHolder*& parkedHolder) override;
    void ExitFreezePark(GilHolder* parkedHolder) override;
    //Frame source resolution behind the view face (see
    //VmExecutorDebug.cpp): execution-window context → its carrier's stack,
    //otherwise the registry's parked stack.
    const std::vector<CallFrame>* FramesOfContext(int contextId) const;

    //Native functions (standard library AND third-party) share the public
    //NativeHost ABI (nlang/vm/NativeHost.h): each call receives a NativeHost
    //function table for all VM access, a return cell, the argument cells and
    //the declared parameter count. A namespace function's args start at
    //slot 0; methods pass the receiver in slot 0. Native code is stateless;
    //lookup is by the CompiledFunction name (a qualified "ns.name" for
    //library functions, a bare name for host-registered natives).
    using NativeFn = ::NativeFn;
    void RegisterNative(const std::string& name, NativeFn fn);
    //K2 slice 4 (§2.5): declare a native that may take unbounded time.
    //The VM stages its arguments/result off the frame, parks the context
    //as BlockedNative and releases the GIL around the call, so other
    //carriers keep running. Duplicate registration of one name through
    //either channel is a load error.
    void RegisterNativeBlocking(const std::string& name, NativeFn fn);

    //Append a directory searched for native modules (nlang_<ns>.dll /
    //libnlang_<ns>.so). The executable directory is searched by default.
    void AddNativeSearchDir(std::string dir);

    //Backtrace captured from the last Execute() call. Empty if execution
    //succeeded without throwing.
    const std::string& Backtrace() const { return m_lastBacktrace; }

    //Extension point ⑤: raise a script-visible base Exception carrying
    //msg (message + backtrace populated by the private raiser). The
    //embed adapter translates host C++ failures into this — the one
    //public door to the private RaiseNlangExceptionBase family.
    [[noreturn]] void RaiseHostException(const std::string& msg);

    //Extension point ③ (spec 2026-10-08 §8): host-held handles are GC
    //roots while registered — the embed adapter pushes a root when a
    //reference Value is created and pops when its last shared copy
    //dies. kind routes the marking: RTK_String values are string-store
    //handles, everything else is a heap idx.
    void PushHostRoot(uint8_t kind, int32_t value);
    void PopHostRoot(uint8_t kind, int32_t value);

    //Non-mutating string read: flattens into a local buffer, never touches
    //the node (safe for const observers). Public because the intrinsic
    //family TUs read string arguments through file-local static helpers
    //that are not member functions.
    std::string StrValCopy(int32_t handle) const;   //frozen view

private:
    //2026-09-26 maintainability split: Execute's per-run phases, in
    //invocation order (definitions in VmExecutor.cpp).
    void ResetPerRunState(const CompiledModule& module);
    void InitStringStore(const CompiledModule& module);
    void InitStructHeap();
    //K1 Slice 6: cache builtin class indexes (Exception family + Thread)
    //from the freshly linked module — extracted from ResetPerRunState
    //for the function size guard.
    void CacheBuiltinClassIndexes(const CompiledModule& module);

    //Frame machine (concurrency K1): NLang calls push frames onto
    //the evaluation stack and one iterative dispatch loop runs them — NLang
    //recursion no longer consumes C++ stack. RunFrames(baseDepth) runs
    //until the stack shrinks back to baseDepth (root window: 0; nested
    //toString/host windows: the depth at their entry). Definitions in
    //VmExecutorOps.cpp.
    void RunFrames(size_t baseDepth);
    //K6 D2: RunFrames is a thin wrapper holding the evaluation-window
    //guard; the frame-machine body lives in RunFramesInner (the body is
    //at the source-size guard's function line ceiling).
    void RunFramesInner(size_t baseDepth);
    //K1 Slice 6: at RunFrames entry, throw the armed join-target failure
    //(a resumed joiner over a FAILED target) inside the handler-search
    //try — no-op unless the carrier armed ResumeThrowExcIdxRef().
    void ConsumeResumeThrow();
    void DispatchFrames(size_t baseDepth);   //inner loop; throws on error
    //Bind a catch handler for the in-flight exception in this frame
    //(tryBlocks walk at the throwing instruction's opPc). Returns false
    //when no clause takes it — the caller unwinds one frame.
    bool TryEnterCatchHandler(CallFrame& frame, int32_t excHeapIdx);
    //Push/pop. PushCalleeFrame stages params from the caller's
    //callParamBase cells and inherits the caller's result cell; the out
    //variants arm the return writeback (bit i = user param i; source
    //frame slot = i + outShift — bound-method delegates shift by 1).
    //Returns the new top frame's locals pointer.
    uint8_t* PushCalleeFrame(const CompiledFunction& callee,
        uint16_t callParamBase, uint32_t outMask, uint8_t outShift);
    uint8_t* PushBoundCalleeFrame(const CompiledFunction& callee,
        int32_t thisIdx, uint16_t callParamBase, uint32_t outMask);
    //Root/toString/host windows. deferSafepoint: callers that stage raw
    //caller-owned handle cells into the frame right after the push must
    //defer the entry safepoint until staging completes — a collection at
    //the gap would see zeroed param slots and kill the not-yet-staged
    //handles (they exist nowhere else; host memory is not scanned). Such
    //callers run CheckGCSafepoint() themselves once staging is done.
    uint8_t* PushRootFrame(const CompiledFunction& func,
        uint8_t* externalResult, bool deferSafepoint = false);
    void PopFrameWithWriteback();           //out writeback, then pop
    //Shared push core (boundThisIdx > 0 = bound staging), and the unwind
    //bookkeeping both exception paths in RunFrames share.
    uint8_t* PushFrameCore(const CompiledFunction& callee,
        uint16_t callParamBase, uint32_t outMask, uint8_t outShift,
        int32_t boundThisIdx);
    void RecordUnwindAndPopFrame();
    //The frame's result cell: the external C++ cell (root/toString/host
    //windows) or a slot in another frame — resolved fresh, never cached
    //across a push (stack growth moves CallFrame objects; the
    //per-frame localsBuf buffer itself is move-stable).
    uint8_t* FrameResultCell(const CallFrame& frame);

    //Host-bridge internals (definitions in VmExecutorHostBridge.cpp):
    //FillDefaults stages the Option-B defaults for a by-name call's
    //missing formals (what a compiler call site would have staged);
    //RunBridgeNativeCall stages the caller-frame ABI copy and invokes
    //CallNative for CallFunctionByIdx's native arm.
    void FillDefaults(const CompiledFunction& callee,
                      uint8_t* frameCells, uint32_t argc);
    void RunBridgeNativeCall(const CompiledFunction& callee,
                             const uint8_t* argCells, uint32_t argc,
                             uint8_t* resultCell);
    void FillOneDefault(const CompiledFunction& callee,
                        const DefaultValueDesc& d, uint8_t* cell);
    //0.9.11 T4: the single folded-constant write site (definition in
    //VmDefaultMaterializer.cpp) — an 8-byte cell (frame slot or heap
    //field slot) takes the descriptor's value. False = no materializable
    //value (RTK_Void sentinel; RTK_Expr/RTK_Unfoldable are function-
    //default-only) and the caller decides.
    bool MaterializeDefaultValue(const DefaultValueDesc& d, uint8_t* cell);

    //private: resolves a host-side field access on an instance slot.
    //Returns the resolved field's declared RTK kind and sets baseOut to
    //the cell holding the field's low half (structs: fieldIndex*2;
    //classes: 1 + fieldIndex*2). Structs read the CompiledStruct record;
    //classes read the VmClassLayout view (0.9.12 T1: the view mirrors
    //the record today, T2 joins the superClassIdx chain). Throws on a
    //non-instance slot or an out-of-range fieldIndex.
    uint16_t ResolveHostField(int32_t heapIdx, uint16_t fieldIndex,
                              size_t& baseOut) const;

    //Opcode case-body helpers (2026-09-26 maintainability split):
    //the dispatch switch in VmExecutorOps.cpp routes every opcode
    //to a one-per-case method; definitions live in the domain TUs
    //(VmExecutorOpsArith/Control/Strings/Calls/Objects.cpp).
    void OpAssertFail(BytecodeReader& reader);
    void OpConstInt32(BytecodeReader& reader, uint8_t* pResult);
    void OpConstFloat(BytecodeReader& reader, uint8_t* pResult);
    void OpConstString(BytecodeReader& reader, uint8_t* pResult);
    void OpVarLocal(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpAssign(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpJump(BytecodeReader& reader);
    void OpJumpIfNot(BytecodeReader& reader, uint8_t* locals);
    void OpSwitch(BytecodeReader& reader);
    void OpCase(BytecodeReader& reader);
    void OpDebugInfo(BytecodeReader& reader, uint16_t opPc);
    void OpEnum_to_str(BytecodeReader& reader, uint8_t* pResult);
    void OpArray_to_str(uint8_t* pResult);
    void OpConcat_str(BytecodeReader& reader, uint8_t* locals);
    void OpEq_str(BytecodeReader& reader, uint8_t* locals);
    void OpNe_str(BytecodeReader& reader, uint8_t* locals);
    void OpStrLen(BytecodeReader& reader, uint8_t* locals);
    //0.7.5 char bridge: s[i] byte read and the code-point iteration step
    //(operand contracts in BytecodeOps.h).
    void OpStrByteAt(BytecodeReader& reader, uint8_t* locals);
    void OpStrForeachStep(BytecodeReader& reader, uint8_t* locals);
    //0.7.5 generalized numeric family (VmExecutorOpsPrim.cpp): handlers
    //read the kind immediate, resolve the registry row, and dispatch
    //through the function-pointer tables in VmPrimOps.h — no per-kind
    //switches in the executor.
    void OpAdd(BytecodeReader& reader, uint8_t* locals);
    void OpSub(BytecodeReader& reader, uint8_t* locals);
    void OpMul(BytecodeReader& reader, uint8_t* locals);
    void OpDiv(BytecodeReader& reader, uint8_t* locals);
    void OpMod(BytecodeReader& reader, uint8_t* locals);
    //0.9.9 shifts: same kind-immediate encoding as the arithmetic family;
    //the count slot carries the result carrier row (resolver casts the
    //right operand), so the width check reads it with that row.
    void OpShl(BytecodeReader& reader, uint8_t* locals);
    void OpShr(BytecodeReader& reader, uint8_t* locals);
    //Raises the named range error when the count slot (read with the
    //carrier row) is negative or reaches the row's bit width.
    void CheckShiftCount(int row, const uint8_t* p);
    void OpNeg(BytecodeReader& reader, uint8_t* locals);
    void OpCmp(BytecodeReader& reader, uint8_t* locals);
    void OpPrimCast(BytecodeReader& reader, uint8_t* pResult);
    void OpPrimToStr(BytecodeReader& reader, uint8_t* pResult);
    void OpConstInt64(BytecodeReader& reader, uint8_t* pResult);
    void OpConstDouble(BytecodeReader& reader, uint8_t* pResult);
    //Registry-driven slot read: sign-extends signed rows by width,
    //zero-extends unsigned rows, truncates float rows first. Used by
    //OpPrimCast's numeric->char code-point validation.
    int64_t ReadScalarAsInt64(int row, const uint8_t* p) const;
    void OpLogicalNot(BytecodeReader& reader, uint8_t* locals);
    //Call family contract (frame machine): return true when a new frame
    //was pushed — the dispatcher must re-fetch EvalStack().back() before
    //the next instruction. False = handled inline (native/intrinsic/
    //diagnostic throw).
    bool OpCallFunc(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpMakeFunc(BytecodeReader& reader, uint8_t* pResult);
    bool OpCallDelegate(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    bool OpCallDelegateOut(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpMakeBoundFunc(BytecodeReader& reader, uint8_t* pResult);
    void OpMakeVFunc(BytecodeReader& reader, uint8_t* pResult);
    void OpFuncEquality(BytecodeReader& reader, uint8_t* locals, OpCode op);
    void OpFunc_to_str(uint8_t* pResult);
    bool OpCallFuncOut(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    bool OpCallMethodDirect(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    bool OpCallMethodDirectOut(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    bool OpCallMethod(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpCallIntrinsic(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpAllocStruct(BytecodeReader& reader, uint8_t* locals);
    void OpLoadField(BytecodeReader& reader, uint8_t* locals);
    void OpStoreField(BytecodeReader& reader, uint8_t* locals);
    void OpNew(BytecodeReader& reader, uint8_t* locals);
    void OpNullCheck(BytecodeReader& reader, uint8_t* locals);
    void OpCopyStruct(BytecodeReader& reader, uint8_t* locals);
    void OpAllocArrayN(BytecodeReader& reader, uint8_t* locals);
    //Stage-3 D3: assemble a rank>=2 array from staged row handles (the
    //dynamic-shape initializer arm); contract at the implementation.
    void OpAssembleArrayN(BytecodeReader& reader, uint8_t* locals);
    //Validation pass of OpAssembleArrayN: null/kind/row-rank gates plus
    //the shared per-level width check, and the handle/dims snapshot the
    //allocation consumes.
    void SnapshotAssembleRows(const uint8_t* locals, uint16_t elemSlotBase,
        uint16_t rowCount, uint8_t rank, std::vector<int32_t>& rowHandles,
        int32_t* dims);
    //Row-payload copy of OpAssembleArrayN (see the implementation).
    void CopyAssembleRows(int32_t heapIdx,
        const CompiledArrayType& arrayType,
        const std::vector<int32_t>& rowHandles);
    void OpLoadElementN(BytecodeReader& reader, uint8_t* locals);
    void OpStoreElementN(BytecodeReader& reader, uint8_t* locals);
    void OpLoadElementFlat(BytecodeReader& reader, uint8_t* locals);
    void OpArrayShape(BytecodeReader& reader, uint8_t* locals);
    void OpArrayLength(BytecodeReader& reader, uint8_t* locals);
    void OpBox(BytecodeReader& reader, uint8_t* pResult);
    void OpUnbox(BytecodeReader& reader, uint8_t* pResult);
    void OpCheckCast(BytecodeReader& reader, uint8_t* pResult);
    void OpThrow(BytecodeReader& reader, uint8_t* locals);
    void OpRethrow();
    void OpPopHandler();

    //Deep-copy a struct from srcHeapIdx to a new heap slot.
    //Returns the new heap index.
    int32_t DeepCopyStruct(int32_t srcHeapIdx, uint16_t structIdx);

    //Serialize all fields of a struct to a byte sink.
    //Recursive over nested struct and class fields (class via
    //SerializeClassFields — Phase 8c). Array fields throw (Phase 8e).
    //Depth limit prevents pathological cycles.
    //Writer callable signature: void(const uint8_t* p, size_t n)
    template<typename Writer, typename StreamState>
    void SerializeStructFields(int32_t heapIdx, uint16_t structIdx,
        Writer&& write, StreamState& st, int depth = 0);

    //Deserialize fields from a byte source into a freshly-allocated struct.
    //Reader callable signature: void(uint8_t* dst, size_t n) — must throw
    //on short read (EOF). Caller must have pre-allocated root struct via
    //AllocStructOnHeap; nested struct slots are allocated here.
    template<typename Reader, typename StreamState>
    void DeserializeStructFields(int32_t heapIdx, uint16_t structIdx,
        Reader&& read, StreamState& st, int depth = 0);

    //Phase 8c class serialization helpers. Declarations only — definitions
    //are added in Task 4. StreamState is templated so the same helper works
    //for both ByteStreamState and FileStreamState without a common base.
    template<typename Writer, typename StreamState>
    void SerializeClassFields(int32_t heapIdx,
        Writer&& write, StreamState& st, int depth);

    template<typename Reader, typename StreamState>
    void DeserializeClassFields(uint16_t declaredClassIdx, int32_t& outHeapIdx,
        Reader&& read, StreamState& st, int depth);

    //Allocate a struct on the heap with recursive nested struct allocation.
    //Returns the heap index.
    int32_t AllocStructOnHeap(uint16_t structIdx);
    //K8 sharding: own-shard lock-free pop, Growth-locked block refill.
    int32_t TakeHeapSlot();
    int32_t TakeStringSlot();
    size_t CurrentShard() const;
    void GrowHeapShardBlockLocked(size_t shard);
    void GrowStrShardBlockLocked(size_t shard);
    //Sweep-only routing of a free heap index to its mapped shard.
    void HeapSweepPush(int32_t idx);

    //Allocate a class object on the heap with recursive field allocation.
    //Returns the heap index.
    int32_t AllocClassOnHeap(uint16_t classIdx);

    //Phase 9b-pre: collection toString helpers. These recurse through
    //nested collections with a depth cap (TOSTRING_DEPTH_LIMIT) to bound
    //output size and prevent runaway recursion on cyclic structures.
    //Returns the formatted result; throws on depth overflow.
    std::string QuoteString(const std::string& s) const;
    std::string FormatHeapValue(int32_t heapIdx, int depth);
    //0.7.5: canonical scalar renderer — one value in little-endian cell
    //form (lo cell + hi cell; narrow rows ignore hi), formatted through
    //the registry's RnBuiltinDataType row, the same single source
    //OP_Prim_to_str uses. Every boxed payload, array element, heap field
    //and debug-view rendering delegates here — one format decision per
    //kind, family-wide (uint renders unsigned; narrow/long tags no
    //longer fall to "<unknown>").
    std::string FormatScalarValue(uint8_t rtk, int32_t lo, int32_t hi) const;
    std::string FormatArrayElemScalar(int32_t elemVal, uint8_t elemKind,
        int depth);
    void FormatArrayElement(std::string& result,
        const std::vector<int32_t>& slot, size_t off, uint8_t elemKind,
        int cells, int depth);
    void FormatArrayNDLevel(std::string& result,
        const std::vector<int32_t>& slot, uint8_t rank, uint8_t level,
        size_t& flatIndex, uint8_t elemKind, int cells, int depth);
    std::string FormatArray(int32_t heapIdx, int depth);
    std::string FormatList(int32_t handle, int depth);
    std::string FormatDict(int32_t handle, int depth);
    //Class index a virtual method call on this receiver dispatches on.
    //A boxed record's header slot holds the wrapped type tag, not a
    //class index — boxed receivers resolve against Object, the static
    //type every boxed value carries (their only reachable methods are
    //the Object protocol). Returns -1 when Object is absent (callers
    //report an invalid class index).
    int32_t ReceiverClassIndex(int32_t heapIdx) const;

    //Invoke virtual toString on a class instance by heap idx, returning
    //the result string. Mirrors OP_CallMethod's vtable walk; used by
    //collection formatters for class-typed elements.
    std::string InvokeVirtualToString(int32_t thisHeapIdx);

    //Execution half of InvokeVirtualToString for a resolved override:
    //synthetic this-only frame, intrinsic dispatch via
    //ExecuteIntrinsic, native records refused (no marshalling), plain
    //methods through a nested RunFrames window. Returns the interned
    //result string (StrVal flattens a cons chain on first read).
    std::string CallToStringOverride(const CompiledFunction& callee,
        int32_t thisHeapIdx, int32_t classIdx);

    //Phase 8d — polymorphism check for class-typed deserialization.
    //Walks superClassIdx chain. Returns true if actualIdx is declaredIdx
    //or a subclass thereof.
    bool IsSubclassOf(uint16_t actualIdx, uint16_t declaredIdx);

    //Allocate an array on the heap.
    //Returns the heap index.
    //The single array-allocation choke point (language opcodes and
    //native minting). Grows m_structHeap via emplace_back, so EVERY
    //reference or pointer into m_structHeap held by the caller is
    //invalidated: read what is needed into locals first, call, then
    //re-index. OpArrayShape once read the source record's dims through
    //a reference held across this call and landed in freed memory.
    int32_t AllocArrayOnHeap(uint16_t arrayTypeIdx, uint8_t rank,
        const int32_t* dims);
    //Per-dim and product validation; raises OutOfBounds. Returns elemCount.
    int32_t ValidateArrayDims(uint8_t rank, const int32_t* dims);

    //GC: mark-sweep garbage collection.
    //Design decisions (see docs/user_manual/en/vm-architecture/garbage-collection-design.md
    //for full rationale):
    //  1. Safepoint-triggered, not allocation-point-triggered.
    //     Why: avoids tracking tempSlot/tempSlot2 references in MarkPhase.
    //  2. Precise scan via LocalDescriptor, not conservative byte scan.
    //     Why: decouples GC from stack frame physical layout (slot stride, alignment).
    //  3. m_slotStructIdx parallel array for struct type identification.
    //     Why: struct objects have no type header; smaller change than adding one.
    //  4. Iterative mark with worklist, not recursive.
    //     Why: avoids stack overflow on deep object chains (e.g. linked lists).
    void CollectGarbage();
    void MarkPhase();
    //2026-09-26 maintainability split: MarkPhase's root scans and per-kind
    //trace arms, extracted as named helpers (definitions in VmExecutorGC.cpp).
    //PushMarked is the mark+enqueue step every arm shares.
    void PushMarked(int32_t idx, std::vector<int32_t>& worklist);
    void MarkFrameLocals(const CallFrame& frame, std::vector<int32_t>& worklist);
    void MarkFrameResult(const CallFrame& frame, std::vector<int32_t>& worklist);
    //Conservative band scan of one frame's evalArea cells (beyond the
    //param slots) — staged call arguments live there with no locals-table
    //entry, so a co-argument's safepoint could otherwise sweep them (the
    //F2 gap). Value-tests each cell in both the string-handle and
    //heap-index domains: over-marking is safe (GC is non-moving), and
    //frame zero-init rejects never-written cells.
    void MarkFrameBand(const CallFrame& frame, std::vector<int32_t>& worklist);
    //Both frame-root surfaces — the published carrier stacks (GcStop
    //registry scan) and the registry ctx->frames — under ONE scheduler
    //lock bracket so frame-ownership transfers cannot slip between the
    //arms (K8 Task 7; see the definition for the invariant).
    void ScanFrameRootSurfaces(std::vector<int32_t>& worklist);
    //K1 Slice 6: scheduler root arm — parked/queued contexts' frames
    //(not on any evaluation stack under the steal model) plus each context's
    //Thread object and a Failed context's exception object (join rethrows
    //it by heap idx). Same three frame arms as the running stack.
    void MarkSchedulerRoots(std::vector<int32_t>& worklist);
    //One context's resultCell: conservative both-domain value test for
    //live contexts (the accumulator of the whole root call tree — K8
    //Task 7), kind-classified by resultKind once finished/failed (K3).
    void MarkContextResultCell(const ExecutionContext* ctx,
                               std::vector<int32_t>& worklist);
    //K8 Task 7 product tripwire: every pending delivery destination
    //(channel waiter resultPtr, parked get() joinResultDst) must fall
    //inside one of the frame buffers the root scan just covered — a
    //destination escaping both surfaces means a frame transfer lost
    //its waiter's frame. Runs inside MarkPhase's transfer-freeze
    //bracket on a consistent range list.
    void AuditWaiterDestinationRanges(
        const std::vector<std::pair<const uint8_t*, const uint8_t*>>&
            frameRanges);
    //K2 slice 4: blocking-native local roots (staged cells and minted
    //handles) — same dual-domain value test as the frame band.
    void MarkNativeCallRoots(std::vector<int32_t>& worklist);
    //Extension point ③: host-registered roots (host-held reference
    //Values) — same dual-domain value test as the frame band.
    void MarkHostRoots(std::vector<int32_t>& worklist);
    //Shared value test behind the frame band and the native roots: a
    //true reference is valid in exactly its own domain (string-store
    //handle vs heap idx); a cross-domain hit can only over-mark.
    void TestRootValue(int32_t val, std::vector<int32_t>& worklist);
    void MarkFuncReceiver(int32_t idx, std::vector<int32_t>& worklist);
    void MarkBoxedPayload(int32_t idx);
    void MarkClassFields(int32_t idx, std::vector<int32_t>& worklist);
    void MarkListElements(int32_t idx, std::vector<int32_t>& worklist);
    void MarkDictEntries(int32_t idx, std::vector<int32_t>& worklist);
    void MarkChannelElements(int32_t idx, std::vector<int32_t>& worklist);
    void MarkStructFields(int32_t idx, std::vector<int32_t>& worklist);
    void MarkArrayElements(int32_t idx, std::vector<int32_t>& worklist);
    void SweepPhase();
    void RecycleClassContainerHandles(size_t heapIdx);
    void FreeOwnedStructs(int32_t heapIdx);
    void FreeNestedStructs(int32_t heapIdx, uint16_t structIdx);
    void FreeOwnedArrayStructElements(int32_t heapIdx);

    //Build a backtrace string from UnwindFrames(). Called when an exception
    //propagates out of main(); format is "  at <func> (<module>.n:<line>)".
    std::string FormatBacktrace() const;

    //Check if GC should run at a safepoint (function entry, loop back-edge).
    void CheckGCSafepoint();
    //K7 site 2 helpers. CollectAndReenter: the elected last arriver's
    //collection plus this thread's re-entry into the rendezvous
    //denominator — exception-balanced (the re-entry runs before any
    //unwind continues; see VmExecutorGC.cpp). CollectAndComplete: the
    //last arriver's epilogue — lock-free collection behind the
    //AssertAllArrived quiescence gate, pressure-flag clear, then an
    //RAII completion (clearing before the completion notify means
    //joiners wake to a cleared flag — no collection storm; an escaping
    //exception still completes the epoch before unwinding). NoteGcPressure:
    //the allocation sites' trigger store (GIL held; synthesizes both
    //threshold domains).
    //GcPressureExceeded: the backedge poll's acquire load. NestDepth /
    //GcPollRootDepth: this thread's slot reads for the poll gate.
    void CollectAndReenter(GcWorldStop& stop);
    void CollectAndComplete();
    void NoteGcPressure();
    bool GcPressureExceeded() const;
    int NestDepth() { return m_spScheduler->CarrierNestDepthRef(); }
    int GcPollRootDepth() {
        return m_spScheduler->CarrierGcPollRootDepthRef();
    }

    //K1 decision 5: back-edge quota side-check (called beside the GC
    //safepoint in OpJump). Budget exhaustion with another runnable
    //context parks this one via ParkSignal(Preempted); alone, the
    //budget reloads and execution continues — single-context programs
    //are bit-identical.
    void CheckSchedulerQuota();

    //Single chokepoint for every suspension request (O2 audit): inside
    //a nested evaluation window (toString / host CallByName) an
    //instruction is mid-flight, so parking is impossible — reject with
    //a catchable base exception instead of ParkSignal. All future
    //producers (quota, thread intrinsics, timer, mutex, channel) must
    //route through here.
    [[noreturn]] void RequestPark(ParkSignal::Reason reason);
    //Pre-park gate (decision 2 spec (a)): park producers that mutate
    //scheduler state BEFORE parking (join/sleep) must pass this first —
    //the nested-window rejection throws and must not leave a half-parked
    //context behind.
    void EnsureSuspensionAllowed() const;

    //K1 Slice 4: one context's root window — Execute's former run body
    //(native entry dispatch, or root-frame push + run to completion).
    void RunRootWindow(const CompiledFunction& entryFunc,
                       uint8_t* resultCell);

    //K1 Slice 4 / K2 slice 1: the carrier loop — runs contexts off the
    //scheduler's FIFO queue until main returns (or the queue drains).
    //K2: one loop iteration = one GIL holding window (INV-1/INV-2); the
    //loop body is shared by the primary carrier and (slice 2) the
    //auxiliary pool threads.
    void CarrierLoop(const CompiledFunction& mainFunc);

    //K2 slice 1: the primary carrier — runs the loop on the Execute
    //thread, then joins the auxiliary pool and rethrows any conflated
    //auxiliary error exactly once. Result routing goes through the
    //scheduler registry (main's ctx->resultCell), so no host cell is
    //passed in.
    void RunPrimaryCarrier(const CompiledFunction& mainFunc);

    //K2 slice 2: launch the auxiliary pool (n-1 threads for a configured
    //count of n; each thread installs its own carrier slot and runs the
    //same loop). JoinAndCollect waits for pool exit with a grace
    //deadline (a carrier stuck inside a host callback past the deadline
    //is detached — a leaked thread beats a hung host), then joins or
    //detaches per thread and clears the pool. RethrowAuxiliaryError is
    //the single rethrow point for the conflated slot.
    void LaunchAuxiliaryCarriers(const CompiledFunction& mainFunc);
    void RunAuxiliaryCarrier(const CompiledFunction& mainFunc, int carrierId);
    void JoinAndCollect();
    void RecordAuxiliaryError();
    void RethrowAuxiliaryError();

    //K1 Slice 6: one context's turn on the carrier — resume (frames
    //return to the carrier's evaluation stack; a failed-join joiner rethrows before
    //dispatch) or start main's fresh root window; on return, record the
    //result and wake joiners. K2: every context, main included, writes
    //its result into ctx->resultCell (registry cell — stable across
    //carriers); the host reads it after the run. May throw anything
    //RunFrames throws.
    void RunContextWindow(int ctx, ExecutionContext* info,
                          const CompiledFunction& mainFunc);

    //K1 Slice 6: arm ResumeThrowExcIdxRef() when this context was woken
    //over a FAILED join target (rethrowOnResume). Refuses loudly on a
    //missing/exceptionless target — never silently no-op.
    void ArmResumeRethrow(ExecutionContext* info);

    //K1 Slice 6: containment rule for an uncaught NLang exception
    //escaping a context root. Spawned: Failed + wake joiners (failure
    //reaches the host only through join; dead frames' unwind records
    //drop). Main: rethrow to the host. Returns true for main.
    bool FailContextContained(int ctx, int32_t excHeapIdx);

    //K2 slice 2: CarrierLoop's catch-all — a non-NLang error is fatal
    //for the whole run: mark the context failed, drop its frames, and
    //rethrow (the backtrace comes from the unwind records instead).
    [[noreturn]] void FailContextFatal(int ctx);

    //K7 site 1: per-dispatch participation scope — the carrier enters
    //the GC rendezvous denominator before executing bytecode and arrives
    //when the dispatch ends. Placement inside CarrierLoop's try block
    //is load-bearing (C++ unwinding rule): try-block automatic objects
    //destruct BEFORE catch bodies, so every throw out of the try — all
    //four catch arms — completes this thread's arrival (possibly
    //collecting inline as the last arriver) while the frames are still
    //on THIS thread's evaluation stack, exactly where the registry's
    //scan arm finds them. A zombie whose entry a Reset retired reads
    //false from both flips and skips (GcWorldStop zombie tolerance).
    class ExecutingScope {
    public:
        explicit ExecutingScope(VmExecutor& exec) : m_rExec(exec) {
            m_rExec.m_spScheduler->GcStop().BeginExecuting();
        }
        ExecutingScope(const ExecutingScope&) = delete;
        ExecutingScope& operator=(const ExecutingScope&) = delete;
        ~ExecutingScope() {
            if (m_rExec.m_spScheduler->GcStop().EndExecuting())
                m_rExec.CollectAndComplete();   //last arriver collects
        }
    private:
        VmExecutor& m_rExec;
    };

    //K7 site 2: release the global lock across a whole rendezvous
    //sequence (CheckGCSafepoint's GIL-holding callers — the spawn tail
    //inside an intrinsic's store guard, guarded function entries). Rule
    //A parks on the rendezvous condition variable and the last arriver's
    //inline collection needs the global lock, so waiting under it
    //deadlocks; the bracket must span EndExecuting -> wait -> collect ->
    //BeginExecuting as a whole. The GilHolder* is saved BEFORE Release
    //clears t_gilOwner (RunBlockingNative's precedent). GIL-free callers
    //construct a no-op.
    class GilHandoffScope {
    public:
        explicit GilHandoffScope(VmExecutor&) {
            if (t_gilOwner != nullptr) {
                m_pHolder = t_gilOwner;
                m_pHolder->Release();
            }
        }
        GilHandoffScope(const GilHandoffScope&) = delete;
        GilHandoffScope& operator=(const GilHandoffScope&) = delete;
        ~GilHandoffScope() {
            if (m_pHolder)
                m_pHolder->Acquire();
        }
    private:
        GilHolder* m_pHolder = nullptr;
    };

    //K7 site 4: host-bridge participation RAII — self-classifying
    //through ExecutingSelf at every bridge entry (the embedder's
    //outermost call, a blocking-native callback's re-entry, nested
    //bridge calls):
    //  unregistered           -> RegisterThread + BeginExecuting (a
    //                            participation root; unregisters at exit)
    //  registered + executing -> no-op (already counted; a Rule A wait
    //                            here would self-deadlock the rendezvous)
    //  registered + not exec. -> BeginExecuting (callback re-entry: the
    //                            blocking arm parked this thread; the
    //                            callback's frames must be scanned
    //                            through this thread's own entry)
    // The flipping branches record a participation-root depth so
    // CheckGCSafepoint keeps polling inside the window — bridge windows
    // run at nestDepth >= 1 (NestEvalGuard increments unconditionally),
    // so a raw "depth > 0" gate would never poll a host-outermost run
    // and every stop snapshotting it executing would hang. Construct
    // BEFORE any global-lock acquisition at the entry (Rule A contract).
    class BridgeExecutingScope {
    public:
        explicit BridgeExecutingScope(VmExecutor& exec) : m_rExec(exec) {
            GcWorldStop& stop = m_rExec.m_spScheduler->GcStop();
            if (stop.ExecutingSelf())
                return;   //nested bridge call — already participating
            m_flipped = true;
            if (!stop.IsRegistered()) {
                m_token = stop.RegisterThread(
                    &m_rExec.m_spScheduler->CarrierEvalStackRef());
                m_registered = true;
            }
            stop.BeginExecuting();
            int& rootDepth =
                m_rExec.m_spScheduler->CarrierGcPollRootDepthRef();
            m_savedRootDepth = rootDepth;
            rootDepth = m_rExec.m_spScheduler->CarrierNestDepthRef() + 1;
        }
        BridgeExecutingScope(const BridgeExecutingScope&) = delete;
        BridgeExecutingScope& operator=(const BridgeExecutingScope&) = delete;
        ~BridgeExecutingScope() {
            if (!m_flipped)
                return;
            GcWorldStop& stop = m_rExec.m_spScheduler->GcStop();
            m_rExec.m_spScheduler->CarrierGcPollRootDepthRef()
                = m_savedRootDepth;
            if (stop.EndExecuting())
                m_rExec.CollectAndComplete();
            if (m_registered)
                stop.UnregisterThread(m_token);
        }
    private:
        VmExecutor& m_rExec;
        GcThreadToken m_token;
        bool m_flipped = false;
        bool m_registered = false;
        int m_savedRootDepth = 0;
    };

    //K2 slice 4 (§2.5 re-entrant callback GIL): acquire the GIL unless
    //this thread already holds it (carrier window or an outer ensure
    //guard). NativeHost callbacks and the by-name bridge enter the VM
    //through here — including from inside a blocking native, where the
    //carrier's window lock is away. K7: the mechanism lives in
    //VmScheduler::GilEnsureGuard now; this entry point keeps the
    //VmExecutor& shape so the re-entrant callback sites are unchanged.
    //Nesting works across both guard families through the shared
    //namespace-scope t_gilOwner.
    class GilEnsureGuard {
    public:
        explicit GilEnsureGuard(VmExecutor& exec)
            : m_guard(*exec.m_spScheduler) {}
        GilEnsureGuard(const GilEnsureGuard&) = delete;
        GilEnsureGuard& operator=(const GilEnsureGuard&) = delete;
    private:
        VmScheduler::GilEnsureGuard m_guard;
    };

    //K4: checkpoints fire for every context (K1 decision 7 lifted) —
    //the hooks see spawned contexts' statements and throws too.
    bool DebugHooksActive() const {
        return m_pDebugHooks != nullptr;
    }

    //K4: apply the hooks' declared focus to the scheduler's exclusive
    //dispatch gate. Called after every checkpoint callback (the front
    //end's resume command settles the desired focus inside the frozen
    //window; stepping holds the gate, Continue releases it). Runs while
    //the carrier still holds the GIL — the scheduler lock discipline.
    void SyncDebugFocus() {
        m_spScheduler->SetDebugFocusContext(
            m_pDebugHooks->DebugFocusContext());
    }

    //Execute a VM-side intrinsic function. Called from OP_CallMethod{,Direct}
    //when callee.intrinsicId != INTR_None.
    void ExecuteIntrinsic(uint16_t intrinsicId, uint16_t callParamBase,
        uint8_t* locals, uint8_t* pResult);

    //Phase 11: the surviving stdlib intrinsic family. The string methods live
    //in their own TU (IntrinsicsString.cpp) with their table in vm/StdLib.h,
    //and return false when the id is not ours — ExecuteIntrinsic then reaches
    //the unknown-id throw. Receiver-dispatched: the receiver string handle is
    //at callParamBase[0] and the arguments start at slot 1 (the string.equals
    //ABI). math/io/fs are not intrinsics: they are library sources whose
    //native members run from nlang_<ns>.dll.
    bool ExecuteIntrinsicContainer(uint16_t intrinsicId,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult);
    //Structural half: caller must hold the Container domain guard
    //(DomainGuard is per-thread reentrant; composite sites may nest).
    bool ExecuteIntrinsicContainerStructural(uint16_t intrinsicId,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult);
    bool ExecuteIntrinsicString(uint16_t intrinsicId, uint16_t callParamBase,
        uint8_t* locals, uint8_t* pResult);

    //2026-09-26 maintainability split: the ByteStream and FileStream
    //families moved out of ExecuteIntrinsic into their own domain TUs
    //(IntrinsicsByteStream.cpp / IntrinsicsFileStream.cpp). Same family
    //contract: return false when the id is not ours.
    bool ExecuteIntrinsicByteStream(uint16_t intrinsicId,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult);
    bool ExecuteIntrinsicFileStream(uint16_t intrinsicId,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult);

    //K1: thread.* free-call intrinsics and the Mutex/Channel method
    //intrinsics (IntrinsicsThread.cpp). Same family contract: return
    //false when the id is not ours. thread.* functions are called via
    //OP_CallFunc — args start at callParamBase slot 0 (no `this`); the
    //sync-primitive methods ride the receiver-dispatched shape (slot 0
    //= this). K8: the family splits at the dispatch — thread.* arms keep
    //the scheduler domain lock (scheduler state + heap minting), while
    //the Mutex/Channel arms take the Sync domain over the side tables
    //(ExecuteIntrinsicSync) and must never hold the scheduler lock
    //across their park/wake calls.
    bool ExecuteIntrinsicThread(uint16_t intrinsicId,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult);
    //The __handle builtin methods (Mutex/Channel/K3 Future —
    //receiver-dispatched, slot 0 = this); ExecuteIntrinsicThread's
    //default arm forwards here (50-line function budget). K8: Future
    //arms keep the scheduler lock; Mutex/Channel forward again to
    //ExecuteIntrinsicSync.
    bool ExecuteIntrinsicHandleMethods(uint16_t intrinsicId,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult);
    //K8: the Mutex/Channel family entry. Structural work (slot
    //resolution, table mutation, waiter registration) runs inside
    //Sync-domain sections in the arms; park/wake scheduler calls run
    //between the sections — never under the domain. NLang raises inside
    //a section defer (DeferredNlangRaise) and convert here.
    bool ExecuteIntrinsicSync(uint16_t intrinsicId,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult);
    //K1 Slice 6 spawn/join bodies (IntrinsicsThread.cpp), extracted from
    //ExecuteIntrinsicThread for the file/function size guards. spawn
    //resolves the func<void> handle, registers the context, builds its
    //root frame (receiver at slot 0 for bound handles) and mints the
    //Thread object; join parks or rethrows per the target's state.
    bool IntrinsicSpawn(uint16_t callParamBase, uint8_t* locals,
                        uint8_t* pResult);
    bool IntrinsicJoin(uint16_t callParamBase, uint8_t* locals);
    //K1 Slice 7: park the current context on a steady_clock deadline;
    // negative durations are rejected as a basic error.
    bool IntrinsicSleep(uint16_t callParamBase, uint8_t* locals);
    //K1 Slice 8 / K8: Mutex method intrinsics (IntrinsicsThread.cpp).
    //lock parks on contention (FIFO waiters, non-reentrant — a
    //self-relock parks and the carrier's exit guard reports the
    //deadlock) via the two-phase park handshake (see
    //ExecuteIntrinsicSync); tryLock never parks; unlock validates
    //ownership and hands off to the front waiter.
    struct MutexSlot;   //defined beside the side-table members below
    bool IntrinsicMutexCtor(uint16_t callParamBase, uint8_t* locals);
    bool IntrinsicMutexLock(uint16_t callParamBase, uint8_t* locals);
    bool IntrinsicMutexUnlock(uint16_t callParamBase, uint8_t* locals);
    bool IntrinsicMutexTryLock(uint16_t callParamBase, uint8_t* locals,
                               uint8_t* pResult);
    //Slice 8 helpers. K8: the allocator owns the Sync-domain guard
    //(free-list pop + growth are structure writes; reentrant for
    //already-inside callers). The handle read is heap-side (lock-free —
    //D2 field reads; raises before any domain is taken); the slot lookup
    //runs under the caller's Sync guard and defers the
    //uninitialized-instance error via a null return.
    int32_t AllocMutexHandle();
    int32_t ReadSyncPrimitiveHandle(uint16_t callParamBase, uint8_t* locals,
                                    const char* callName);
    MutexSlot* SyncMutexSlotAt(int32_t handle);
    //K8: the recheck half of the mutex handshake — commit a kPending
    //entry (RequestPark follows, [[noreturn]]) or consume a kAcquired
    //stamp (the caller aborts the migration).
    struct SyncWaiter;   //defined beside the side-table members below
    void ConsumeMutexParkStamp(int32_t handle, int selfId);
    //P2-1: the stamp-consumption core shared by the Mutex/Semaphore
    //recheck halves — both stores hold SyncWaiter deques.
    void ConsumeSyncWaiterStamp(std::deque<SyncWaiter>& waiters,
                                int selfId, ParkSignal::Reason reason);
    //P2-1 (v0.10.17): Semaphore method intrinsics
    //(IntrinsicsSemaphore.cpp). acquire parks FIFO at zero permits (the
    //two-phase handshake, Mutex shape — release hands the permit to
    //the front waiter instead of counting up); tryAcquire never parks;
    //release with no waiter counts up (over-release is legal).
    struct SemaphoreSlot;   //defined beside the side-table members below
    bool IntrinsicSemaphoreCtor(uint16_t callParamBase, uint8_t* locals);
    bool IntrinsicSemaphoreAcquire(uint16_t callParamBase, uint8_t* locals);
    bool IntrinsicSemaphoreRelease(uint16_t callParamBase, uint8_t* locals);
    bool IntrinsicSemaphoreTryAcquire(uint16_t callParamBase,
                                      uint8_t* locals, uint8_t* pResult);
    //The allocator takes the initial count (the AllocChannelHandle
    //capacity shape: the slot write shares the allocation's Sync
    //section — a concurrent growth reallocates the store vector).
    int32_t AllocSemaphoreHandle(int initialCount);
    //P2-1: the semaphore store lookup + recheck half (the
    //ConsumeMutexParkStamp shapes over m_semaphoreStore).
    SemaphoreSlot* SyncSemaphoreSlotAt(int32_t handle);
    void ConsumeSemaphoreParkStamp(int32_t handle, int selfId);
    //K1 Slice 9 / K8: Channel method intrinsics (IntrinsicsChannel.cpp).
    //send parks when bounded and full, recv parks on an empty open
    //channel (the result is written at WAKE time through the saved
    //destination pointer — the parked call is not re-executed on resume,
    //the pc-advance invariant); both ride the two-phase handshake. close
    //is idempotent and wakes every waiter (buffered values drain first;
    //the rest wake with the catchable closed exception).
    struct ChannelWaiter;  //defined beside the side-table members below
    struct ChannelSlot;    //same
    bool IntrinsicChannelCtor(uint16_t callParamBase, uint8_t* locals);
    bool IntrinsicChannelSend(uint16_t callParamBase, uint8_t* locals);
    bool IntrinsicChannelRecv(uint16_t callParamBase, uint8_t* locals,
                              uint8_t* pResult);
    bool IntrinsicChannelClose(uint16_t callParamBase, uint8_t* locals);
    //Same side-table discipline as the Mutex helpers above; the channel
    //allocator takes the initial capacity (the minted handle is not yet
    //visible, but the slot write shares the growth section).
    int32_t AllocChannelHandle(int32_t capacity);
    ChannelSlot* SyncChannelSlotAt(int32_t handle);
    //K8: the recheck verdict of the two-phase handshake — the stamped
    //outcome a channel aborter self-serves at its call site (kValue:
    //the deliverer already wrote pResult / moved the value into the
    //buffer; kClosed: raise the closed exception after the release).
    enum class AbortOutcome : uint8_t { kValue, kClosed };
    //K8: the recheck halves. Under Sync: our entry still kPending →
    //commit (mark kCommitted; RequestPark follows — [[noreturn]]).
    //Stamped → pop the entry, abort the migration, return the outcome.
    //A missing entry is a corrupt-table tripwire (each entry has
    //exactly one consumer by construction).
    AbortOutcome CommitOrAbortChanRecv(int32_t handle, int selfId);
    void CommitOrAbortChanSend(int32_t handle, int selfId);
    //K8: send's handoff to the first live waiter (see
    //IntrinsicsChannel.cpp); recv's slot-free staging of the head live
    //pending value into the buffer (returns the committed sender to
    //wake after the release, -1 when none — a pending sender gets
    //stamped kValue).
    bool HandoffToLiveWaiter(ChannelSlot& slot, int32_t value,
                             int& wakeRecv);
    int StagePendingSend(ChannelSlot& slot);
    //K8: close's exception arms — one prebuilt object per audience
    //(recv waiters / pending senders), armed inside the wake call's
    //scheduler critical section (rooted on the context via
    //resumeExcHeapIdx; the GC traces that field). Committed audiences
    //only — stamped parkers self-serve their stamps.
    void WakeClosedWaiters(const std::vector<int>& recvs,
                           const std::vector<int>& senders);
    //K8: close's Sync-section sweeps — stamp pending parkers / collect
    //committed audiences (see IntrinsicsChannel.cpp).
    void CollectClosedRecvOutcomes(ChannelSlot& slot,
                                   std::vector<int>& valueWoken,
                                   std::vector<int>& closedRecvs);
    void CollectClosedSenders(ChannelSlot& slot,
                              std::vector<int>& closedSenders);
    //K1 Slice 6 spawn helpers: validate + resolve the delegate target
    //(no native/intrinsic bodies, zero-argument contract) and mint the
    //Thread object for a registered context (__handle = context id).
    const CompiledFunction& ResolveSpawnTarget(
        const std::vector<int32_t>& handle, int32_t& receiverIdx);
    int32_t MintThreadObject(int ctxId);
    //K3: Future<T> — same receiver shape as the Thread face (slot 1 =
    //context id, no side table) plus the isDone membership probe. get()
    //shares the resolved context: Finished copies the result cell, Failed
    //rethrows the target's exception, otherwise the caller parks on the
    //join mechanism with a result destination for wake-time delivery.
    int32_t MintFutureObject(int ctxId);
    const ExecutionContext& ResolveFutureContext(uint16_t callParamBase,
                                                 uint8_t* locals);
    bool IntrinsicFutureIsDone(uint16_t callParamBase, uint8_t* locals,
                               uint8_t* pResult);
    bool IntrinsicFutureGet(uint16_t callParamBase, uint8_t* locals,
                            uint8_t* pResult);
    //Rethrow a failed join target's original exception object (by heap
    //idx — same object, same raise-time backtrace). Shared by the join
    //intrinsic (target already Failed) and the resume path.
    [[noreturn]] void ThrowJoinTargetFailure(int32_t excHeapIdx);

    //Phase 11 Step 3: allocate one boxed-value heap slot (layout per
    //OP_Box: slot[0]=tag, slot[1]=value bits). Shared by OP_Box and the
    //string split / fs.listFiles intrinsics.
    int32_t AllocBoxedValue(uint8_t typeTag, int64_t val);

    //Phase 13: allocate one function-handle heap record (3 slots:
    //[0]=target, [1]=this, [2]=form; m_slotKinds=RTK_Func). Always
    //allocates — handles are never interned, two references to the same
    //function are distinct records compared by content.
    int32_t AllocFuncRecord(int32_t target, int32_t thisIdx, int32_t form);

    //Phase 13: render a function handle for toString / container
    //formatting ("func <name>" for static handles, "method <name>" for
    //virtual-dispatch handles). Shared by OP_Func_to_str and
    //FormatHeapValue.
    std::string FormatFuncHandle(int32_t heapIdx) const;

    //Phase 13 Step 2: resolve a method by name on a runtime class —
    //walks the class's methodIndices then up the superClassIdx chain
    //(the OP_CallMethod lookup, extracted so virtual-dispatch handles
    //can share it). Returns a functions[] index or -1.
    int FindMethodByName(int classIdx, const std::string& methodName) const;

    //Phase 13 Step 2: shared engine for OP_CallDelegate /
    //OP_CallDelegateOut. Reads the handle's form, resolves the target
    //(static funcIdx or by-name on the runtime class), builds the callee
    //frame (free functions copy args from callParamBase verbatim; bound
    //handles place the captured receiver at frame slot 0 and shift the
    //args by one), executes, and for outMask != 0 copies each marked
    //user-parameter slot back to the caller (reversing the shift for
    //bound handles). Frame-machine contract: returns true when a callee
    //frame was pushed.
    bool ExecuteDelegateCall(const std::vector<int32_t>& handle,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
        uint32_t outMask);

    //Delegate-target resolution half of ExecuteDelegateCall: virtual
    //handles resolve by name on the receiver's runtime class (override
    //chain), static handles are a range-checked functions[] index.
    int ResolveDelegateTarget(const std::vector<int32_t>& handle);

    //Free-function delegate ABI (identical to OP_CallFunc, natives
    //included); the out write-back copies slots unshifted.
    bool CallDelegateFree(const CompiledFunction& callee,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
        uint32_t outMask);

    //Bound-method delegate ABI: the captured receiver rides at callee
    //slot 0, caller args shift right by one; the out write-back reads
    //frame slot i+1 and stages back to callParamBase+i, reversing the
    //shift.
    bool CallDelegateBound(const CompiledFunction& callee, int32_t thisIdx,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
        uint32_t outMask);

    //Phase 9f: shared native-table dispatch for OP_CallFunc and the method
    //call paths (a native method receives `this` at args[0], mirroring the
    //bytecode calling convention). Throws when the host never registered
    //the name — failing at the call site rather than executing the
    //declaration's empty bytecode.
    void CallNative(const CompiledFunction& callee, uint16_t callParamBase,
        uint8_t* locals, uint8_t* pResult);

    //K2 slice 4 (§2.5): the blocking-native arm. `invoke` runs the
    //native (or the host hook) against the staged cells and returns
    //whether the call was handled (the host hook may decline). The arm
    //stages args, arms local roots, steals the frame stack into the
    //context, releases the GIL through the current holder, invokes,
    //reacquires, truncates the roots to the watermark, honors the stop
    //flag (ShutdownSignal passthrough — frames stay parked), restores
    //the frames, and memcpy's the staged result into pResult.
    using BlockingInvoke =
        std::function<bool(const uint8_t* args, uint32_t argc,
                           uint8_t* ret)>;
    bool RunBlockingNative(const CompiledFunction& callee,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
        const BlockingInvoke& invoke);
    //Steal the evaluation stack into the context (BlockedNative) / restore
    //exactly the stolen tail (Runnable-held). Split out of
    //RunBlockingNative for the source-size guard; park also raises
    //t_blockingNativeDepth (the restore lowers it in the caller,
    //next to the GIL reacquire).
    void ParkFramesForBlockingNative(ExecutionContext* ctx);
    void RestoreStolenFrames(ExecutionContext* ctx, size_t stolenDepth);
    //Root a handle minted by a NativeHost callback — no-op outside a
    //blocking section (see t_blockingNativeDepth). Called under the GIL.
    void RootMintedNativeHandle(int32_t handle);
    //K8: stage a blocking arm's ret/args cells into the root registry
    //(under the root-registry mutex).
    void StageNativeRoots(const uint8_t* ret, const uint8_t* args,
                          uint32_t argCount, uint64_t owner);
    //K6 D11: erase exactly one arm's root entries (staged cells + its
    //callbacks' minted values) — replaces the old watermark truncation.
    void RemoveNativeRoots(uint64_t owner);
    //K6: the reacquire tail of RunBlockingNative (split for the source-
    //size guard): root teardown by owner identity, the run-generation
    //zombie guard (K6 D9), stop passthrough, and frame restore. Throws
    //ShutdownSignal when the run ended under the native (frames stay
    //parked; the caller's staged cells die with its stack).
    void FinishBlockingNative(GilHolder& owner, uint64_t rootOwner,
        uint64_t savedRootOwner, ExecutionContext* ctx, size_t stolenDepth);

    //NativeHost ABI plumbing (definitions in VmExecutorNativeHost.cpp).
    int32_t BuildListString(const char* const* items, int count);
    NativeLibraryLoader& EnsureNativeLoader();
    void EnsureNativeAvailable(const std::string& name);
    //NativeHost function-table callbacks (static; they restore the
    //VmExecutor through the VmNativeHost wrapper whose first member is the
    //public table).
    static const char* NativeGetString(NativeHost* self, int32_t handle);
    static int32_t NativeNewString(NativeHost* self, const char* utf8);
    static int32_t NativeNewListString(NativeHost* self,
        const char* const* items, int count);
    static void NativeWriteOutput(NativeHost* self, const char* text);
    static void NativeWriteError(NativeHost* self, const char* text);
    static const char* NativeReadLine(NativeHost* self);
    static const char* NativeReadToken(NativeHost* self);
    static int NativeReadChar(NativeHost* self, uint32_t* outChar);
    static int NativeHasInput(NativeHost* self);

    //Lazily built on the first input native call (stdin's first touch).
    //One executor runs one program once, so the lazily-built view is
    //never rebuilt.
    TokenView& EnsureInputView();
    static void NativeRaiseException(NativeHost* self, int exceptionKind,
        const char* message);
    static uint32_t NativeNextRandom(NativeHost* self);
    static void NativeSeedRandom(NativeHost* self, int32_t seed);
    //v6 array side (contract in NativeHost.h): pinned payload view and
    //zero-initialized minting for scalar-element arrays.
    static int NativeGetArrayView(NativeHost* self, int32_t handle,
        NativeArrayView* out);
    static int32_t NativeNewArray(NativeHost* self, uint8_t elemKind,
        int32_t length);
    //v9 ND side of minting (contract in NativeHost.h): shaped mint for the
    //matrix-family kernels.
    static int32_t NativeNewArrayShaped(NativeHost* self, uint8_t elemKind,
        int32_t rank, const int32_t* dims);
    //Locate the scalar array-type record for (elemRtk, rank) in the linked
    //module's arrayTypes (elemTypeIdx == 0xFFFF). Lookup-only: m_currModule
    //is const and the stdlib TU inlining registers every declared array
    //type at compile time — a miss is an invariant violation, not an
    //appendable state.
    uint16_t FindScalarArrayTypeIdx(uint8_t elemRtk, uint8_t rank) const;

    //Allocate a handle from the ByteStream side table. Returns 1-based handle.
    int32_t AllocByteStreamHandle();
    //Allocate a handle from the FileStream side table. Returns 1-based handle.
    int32_t AllocFileStreamHandle();
    //Phase 8e-3: allocate a handle from the List<T> side table. Returns 1-based.
    int32_t AllocListHandle();
    //Phase 8e-3 fix-up: read this.__handle from callParamBase[0] for a List
    //intrinsic. Validates this-heap-idx, handle, and upper bound. Returns the
    //1-based handle. methodName is used in error messages.
    int32_t ReadListHandle(uint16_t callParamBase, uint8_t* locals,
        const char* methodName);
    //Phase 8e-4: Dict<K,V> side table operations. Mirror List's shape.
    int32_t AllocDictHandle();
    int32_t ReadDictHandle(uint16_t callParamBase, uint8_t* locals,
        const char* methodName);
    //Phase 8e-4: kind-aware key equality. Branches on m_slotKinds[k]:
    //  RTK_Class/RTK_Struct → heap-idx identity
    //  RTK_Boxed            → BoxedValuesEqual (below)
    //Returns false when either idx is out of bounds or kind mismatch.
    bool DictKeysEqual(int32_t k1, int32_t k2) const;

    //0.7.5: full-payload equality for two RTK_Boxed records. Shared by
    //DictKeysEqual and FindListElement — both previously read only the
    //low value cell, which made 4294967295 == -1 true once 8-byte rows
    //(long/ulong) boxed into 3-cell records {tag, lo, hi}. Strings
    //compare by content (StrValCopy, the frozen-view equality path);
    //float keeps IEEE value-bit equality (NaN≠NaN) via the lo cell;
    //8-byte rows compare both cells.
    bool BoxedValuesEqual(int32_t a, int32_t b) const;

    //Phase 8c: clear per-stream object-ID tables. Called from BS_Reset,
    //BS_Close, and FS_Close so subsequent operations start with a fresh
    //identity table. Templated on StreamState so it works for both
    //ByteStreamState and FileStreamState without a common base.
    template<typename StreamState>
    static void ClearObjIdState(StreamState& st) {
        st.serializeObjIds.clear();
        st.deserializeObjIds.clear();
        st.nextObjId = 1;
    }

    static const size_t RECURSE_LIMIT = 1000;
    static const size_t GC_THRESHOLD_DEFAULT = 1024;
    static const size_t STRUCT_SERIALIZE_DEPTH_LIMIT = 64;
    static const size_t TOSTRING_DEPTH_LIMIT = 64;  //Phase 9b-pre: collection toString cycle/DoS bound
    //DoS hardening for untrusted streams: cap string/class-name length so a
    //garbage or malicious length prefix (e.g. 2 GiB) cannot trigger OOM.
    static const size_t MAX_STRING_LENGTH = 16 * 1024 * 1024;  // 16 MiB
    const CompiledModule* m_currModule = nullptr;
    //Runtime instance-field layout view (0.9.12 T1): rebuilt whenever
    //InitializeForRun binds a new module; consumers read class fields
    //through it instead of the CompiledClass parallel arrays.
    VmClassLayout m_classLayout;
    //String objectization: runtime strings are GC-managed immutable objects.
    //An RTK_String slot holds a 1-based handle into m_stringObjs (0 = null /
    //uninitialized, reads as "" — same fallback shape the old pool-index-0
    //path had). Compile-time constants are eagerly materialized as immortal
    //Flat objects at Execute() start (JVM constant-pool semantics).
    struct StrObj {
        enum class Form : uint8_t { Flat, Cons };
        Form form = Form::Flat;
        bool immortal = false;   //constant materialization; never swept
        bool interned = false;   //in the short-string table (== fast path)
        std::string str;         //Flat: content; Cons: unused
        int32_t left = 0, right = 0;  //Cons: child handles (0 = empty side)
    };
    ChunkedArray<StrObj> m_stringObjs;
    //Parallel to m_stringObjs — kept in step at exactly three sites:
    //GrowStrShardBlockLocked (the growth arm), MarkPhase's head reset,
    //SweepStrings' tail reset.
    std::vector<bool> m_strMarkBits;
    ShardFreeList m_strShards;
    std::vector<int32_t> m_constStrCache;  //constant idx -> immortal handle
    int32_t m_emptyStrHandle = 0;          //dedicated immortal ""
    //Name → host function table for native calls. The flag carries the
    //K2 slice 4 blocking declaration (registration channel); non-blocking
    //natives run with the GIL held as before.
    //GIL removal batch 1 (§7.2) invariant: while frames execute this
    //table is read-only. Writes belong to registration time — host
    //RegisterNative* before the run, and the run-init prewarm in
    //InitializeForRun (which tolerates unavailable modules so the
    //first call keeps its lazy diagnostic).
    struct NativeEntry {
        NativeFn fn = nullptr;
        bool blocking = false;
    };
    std::unordered_map<std::string, NativeEntry> m_natives;
    //K2 slice 4 (§2.5): local roots live during a blocking native call
    //(JNI local-ref shape). Address entries cover the staged arg/ret
    //cells on the carrier's C++ stack (the native writes them after
    //entry, so only address rooting sees the final values); value
    //entries cover handles minted by NativeHost callbacks mid-call
    //(newString et al.) before the native copies them into ret. All
    //mutations happen under the GIL; each blocking arm removes
    //exactly its own entries on every exit path after reacquiring
    //(K6 D11: concurrent arms interleave pushes, so position-based
    //truncation could erase a LIVE arm's roots).
    struct NativeCallRoot {
        const uint8_t* cells = nullptr;  //address form base (null = value)
        uint32_t cellCount = 0;          //address form: 8-byte cell count
        int32_t value = 0;               //value form: minted handle
        uint64_t owner = 0;              //K6 D11: the blocking arm that
                                         //pushed this (0 = never valid)
    };
    std::vector<NativeCallRoot> m_nativeCallRoots;
    //K8: host-boundary resources are not stores, so they take small
    //dedicated leaf mutexes instead of domain locks. This one guards
    //both root registries (native call roots and host roots) — pushes,
    //staging and removal only; the collector reads at rendezvous.
    mutable std::mutex m_rootRegistryMutex;
    //Depth of blocking-native sections entered on this thread. Minting
    //callbacks root their handles only while this is non-zero (a
    //non-blocking native holds the GIL across the whole call, so no GC
    //can intervene between mint and the native writing the handle).
    static thread_local int t_blockingNativeDepth;
    //K6 D11: the innermost blocking arm on this thread — callback-minted
    //value roots attribute to it (nested arms save/restore around their
    //invoke). 0 outside any blocking section.
    static thread_local uint64_t t_currentNativeRootOwner;
    //Lazy on-demand loader for native modules (standard-library DLLs and
    //third-party modules use it). Created on first native-module need.
    std::unique_ptr<NativeLibraryLoader> m_upNativeLoader;

    //Last captured backtrace (filled by Execute's catch block).
    std::string m_lastBacktrace;

    //K2 slice 1: confluence slot for an auxiliary-carrier error (slice 2
    //writes it; the primary carrier rethrows it exactly once in
    //JoinAndCollect). Backtrace snapshotted at the capture site because
    //the unwind records live on the capturing thread's slot (K7) —
    //the primary carrier cannot read them after the fact.
    std::exception_ptr m_auxError;
    std::string m_auxBacktrace;

    //ndb: debugger front end (null = disabled).
    IDebugHooks* m_pDebugHooks = nullptr;

    //Host I/O sink (null = write stdout / read stdin as before).
    IHostIo* m_pHostIo = nullptr;

    //Extension point ⑤: owned by the embed adapter (HostFunctionTable);
    //never null-deref'd — CallNative tests before asking.
    IHostFunctions* m_pHostFunctions = nullptr;

    //Extension point ③: host-registered roots as (kind, value) pairs —
    //see PushHostRoot. Small by construction (host-held references);
    //linear pop is fine.
    struct HostRoot { uint8_t kind; int32_t value; };
    std::vector<HostRoot> m_hostRoots;

    //Input side: the source is the host seam when a host is installed,
    //else the portable stdin reader; TokenView owns the shared cursor.
    std::unique_ptr<LineSource> m_upInputSource;
    std::unique_ptr<TokenView> m_upInputView;
    //K8: the console conversation mutexes (host-boundary, not stores).
    //Input serializes the lazy view init and the cursor state; output
    //serializes host callbacks that are not promised thread-safe. They
    //are separate so a carrier blocked at a prompt does not stall
    //another carrier's prints.
    mutable std::mutex m_consoleInMutex;
    mutable std::mutex m_consoleOutMutex;

    //ndb: functions[] index of the innermost frame. Module functions
    //vector is stable after load (no reallocation), so pointer
    //difference is valid.
    uint16_t CurrentFuncIdx() const;

    //ndb: OnThrow checkpoint fired from every NLangThrow raise site
    //(built-in RaiseNlangException, OP_Throw, OP_Rethrow) before the
    //throw, so the full NLang stack is still alive. pc/line use the
    //frame's current statement anchor — a raise has no OP_DebugInfo of
    //its own. Definition in VmExecutorDebug.cpp.
    void FireOnThrow();

    //ndb: shallow value formatters + view plumbing. Definitions live in
    //VmExecutorDebug.cpp (separate TU: the executor core only gains
    //checkpoint calls, all inspection logic stays out of it). All
    //const; none may execute NLang code or touch the NLang heap.
    std::string FormatDebugLocalSlot(const LocalDescriptor& ld,
        const uint8_t* frameLocals) const;
    std::string FormatDebugHeapValue(int32_t heapIdx) const;
    std::string FormatDebugClassInstance(int32_t heapIdx) const;
    std::string FormatDebugStructInstance(int32_t heapIdx) const;
    std::string FormatDebugArray(int32_t heapIdx) const;
    std::string FormatDebugList(int32_t handle) const;
    std::string FormatDebugDict(int32_t handle) const;
    //One field/element cell by declared kind, registry stride aware:
    //8-byte scalar kinds (long/ulong; double with Task 7) read cells
    //[cellIdx, cellIdx+1]. Single renderer for class/struct fields,
    //array elements and boxed payloads (the tag doubles as the
    //declared kind there).
    std::string FormatDebugField(const std::vector<int32_t>& slot,
        size_t cellIdx, uint16_t declaredKind) const;
    std::string FormatDebugElementHeap(int32_t heapIdx) const;
    std::string FormatDebugRefShort(int32_t heapIdx) const;
    std::string FormatDebugBoxed(const std::vector<int32_t>& slot) const;
    std::string FormatDebugStringIdx(int32_t idx) const;

    //String object store (definitions in VmExecutorStrings.cpp).
    //By value: rvalue products (to_string temporaries, concat/readFile
    //buffers moved at their call sites) arrive with zero copies; lvalue
    //callers pay one copy at the call boundary and the body then moves
    //into the slot — never worse than the old const& form.
    int32_t MintNewString(std::string content);              //runtime mint, interns <=kShortStringMaxBytes
    int32_t MintConstantString(const std::string& content);  //immortal flat (intern path + immortal bit)
    bool IsInternedString(int32_t handle) const;             //live + in the short-string table
    bool StringsEqual(int32_t hA, int32_t hB);               //single source for OP_Eq_str / OP_Ne_str
    int32_t AllocConsString(int32_t left, int32_t right);    //O(1) zero-copy node
    int32_t AllocStringObj();                                //raw slot, sets m_gcPressure
    bool IsLiveStringHandle(int32_t handle) const;
    //Requires an Execute()-initialized store (m_emptyStrHandle); all
    //callers are mid-execution today.
    const std::string& StrVal(int32_t handle);               //execution path (flattens in place)
    //Shared cons-subtree walk behind both accessors (iterative; deep
    //left-leaning chains stay stack-safe).
    void FlattenInto(std::string& out, int32_t handle) const;
    void MarkString(int32_t handle);                         //traces cons children
    void SweepStrings();                                     //Task 2

    //Struct heap: each slot is a vector of int32 values (one per field).
    //Index 0 is a sentinel (empty slot).
    using StructSlot = std::vector<int32_t>;
    ChunkedArray<StructSlot> m_structHeap;

    //Payload base of a heap array record, derived from the record's
    //own rank cell: callers cannot pair a record with a foreign rank
    //(hardcoded rank-1 reads once corrupted rank >= 3 shapes). A
    //corrupt record throws here, at the single derivation point,
    //instead of mis-reading neighbouring cells.
    static size_t ArrayPayloadBase(const StructSlot& record)
    {
        if (record.size() < 4)
            throw std::runtime_error(
                "NLang VM: corrupt array record (truncated header)");
        const int32_t rank = record[2];
        if (rank < 1 || rank > kMaxArrayRank)
            throw std::runtime_error(
                "NLang VM: corrupt array record (rank out of range)");
        //The header includes the dims run (and its alignment pad cell):
        //a record shorter than the header has a truncated dims section,
        //and every dims reader (strides, format, GC) would walk past it.
        const size_t base =
            ArrayHeaderCells(static_cast<uint8_t>(rank));
        if (record.size() < base)
            throw std::runtime_error(
                "NLang VM: corrupt array record (truncated dimensions)");
        return base;
    }

    //GC infrastructure.
    ChunkedArray<uint8_t>  m_slotKinds;      //parallel to m_structHeap: RTK_Class/RTK_Struct/0=free
    ChunkedArray<uint16_t> m_slotStructIdx;  //parallel to m_structHeap: struct CompiledStruct index (class uses slot[0])
    std::vector<bool>     m_markBits;       //parallel to m_structHeap: GC mark bit
    ShardFreeList m_heapShards;   //K8: per-carrier free heap slot shards
    DomainLockSet m_domainLocks;  //K8: Growth (later: Container/Sync/Stream/StrMaint)
    //K8: slots appended per growth event. DEFAULT_SHARD_BLOCK in
    //production (mimalloc-style batch refill); 1 under gc-stress so the
    //store stays exactly as large as its population.
    size_t m_allocQuantum = DEFAULT_SHARD_BLOCK;
    size_t m_gcThreshold = GC_THRESHOLD_DEFAULT;
    //K7: atomic trigger flag — allocation sites store the synthesized
    //threshold condition under the GIL (release); the GIL-free backedge
    //poll only loads (acquire) and must never read the store sizes
    //unlocked. Cleared in CollectAndComplete (GIL held, after the
    //collection, before Complete).
    std::atomic<bool> m_gcPressure{false};
    std::atomic<int> m_gcRendezvousParks{0};  //K7: parked arrivals (per run)
    std::atomic<bool> m_gcThrowOnceForTests{false};  //K8: injected failure
    size_t m_gcCollections = 0;    //K6 D7: per-run collection count
    bool   m_conservativeBandForTests = false;  //K2 slice 5 A/B knob

    //String-store GC plumbing (collection activates in Task 2).
    static const size_t kStrGcThresholdDefault = 1024;
    size_t m_strGcThreshold = kStrGcThresholdDefault;
    static const size_t kShortStringMaxBytes = 40;  //Lua short-string cutoff
    std::unordered_map<std::string, int32_t> m_shortStrTable;  //weak: content -> live handle
    static constexpr uint8_t kStrFormDead = 0xFF;   //free-slot sentinel

    //K7 frame privatization: the evaluation stack moved to the carrier
    //slot (running frames belong to the thread executing them). These
    //accessors route to the current thread's slot — the carrier's own
    //storage, or the fallback slot for host threads re-entering through
    //the bridge. The steal protocol is unchanged: parking moves frames
    //into ctx->frames, resume moves them back. Deliberately const (the
    //slot is thread state reached through the scheduler pointer, not
    //executor-object state; const debug views read it) and returning a
    //mutable ref for the same reason.
    std::vector<CallFrame>& EvalStack() const { return m_spScheduler->CarrierEvalStackRef(); }
    //Unwind records + resume-rethrow arming are per-thread companion
    //state for the same reason they were executor-global before: only
    //one evaluation window runs at a time per thread.
    std::vector<UnwindFrame>& UnwindFrames() const { return m_spScheduler->CarrierUnwindFramesRef(); }
    int32_t& ResumeThrowExcIdxRef() const { return m_spScheduler->CarrierResumeThrowExcIdxRef(); }

    //K1 decision 5: the single-carrier scheduler. The running context's
    //frames ARE the slot's evalStack (a park steals them into the context — see
    //VmScheduler.h's file header).
    std::unique_ptr<VmScheduler> m_spScheduler =
        std::make_unique<VmScheduler>();

    //Back-edge quota state (K1 decision 5; K2 slice 2 carrier-ized).
    //Trip limit = configured budget (test knob survives ResetPerRunState);
    //the budget itself lives on the running carrier's scheduler slot
    //(_several carriers are in flight at once — a single member would
    //serialize them wrongly_), reset per dispatch; fire/skip counters
    //feed the white-box tests. Nested evaluation windows skip the quota
    //entirely (a park there is impossible — see RequestPark).
    int m_schedTripLimit = DEFAULT_SCHEDULER_TRIP_BACKEDGES;
    //K7: incremented on the back-edge fast path outside the GIL.
    std::atomic<int> m_quotaFires{0};
    std::atomic<int> m_preemptSkips{0};
    //K2 slice 2: the auxiliary carrier pool. One record per launched
    //thread; exited is shared_ptr so a detached thread (grace deadline
    //hit mid-host-callback) still owns its flag safely.
    struct AuxCarrier {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> exited;
    };
    std::vector<AuxCarrier> m_auxCarriers;
    uint32_t m_carrierThreadCount = 1;   //plan ruling P1: library default 1
    std::mutex m_auxExitMutex;
    std::condition_variable m_auxExitCv;
    //K1 Slice 6: a pending exception object to throw at the next RunFrames
    //entry (a joiner resumed after its FAILED target rethrows the target's
    //original exception INTO the restored frame stack, so the joiner's own
    //try/catch — and the uncaught path — see it exactly as if the join
    //call had thrown). 0 = none; consumed at entry. Armed by the carrier
    //only (top-level resume, nestDepth 0). K7: lives on the carrier slot
    //as ResumeThrowExcIdxRef().
    //K1 Slice 6: cached "Thread" class index (spawn mints instances).
    int16_t m_threadClassIdx = -1;
    //K3: cached "Future" class index (typed spawn mints instances).
    int16_t m_futureClassIdx = -1;

    //Phase 9d: cached class indices for the built-in Exception hierarchy.
    //Initialized in Execute() via module.FindClass; -1 = not found (only
    //possible if the runtime is launched against a malformed module that
    //somehow lacks the built-in registrations).
    int16_t m_exceptionClassIdx = -1;
    int16_t m_nullPtrExcClassIdx = -1;
    int16_t m_divZeroExcClassIdx = -1;
    int16_t m_invalidCastExcClassIdx = -1;  //v0.8.5: InvalidCastException
    int16_t m_oobExcClassIdx = -1;
    int16_t m_assertExcClassIdx = -1;
    int16_t m_ioExcClassIdx = -1;       //Phase 11: IOException
    int16_t m_channelClosedExcClassIdx = -1;  //K3: ChannelClosedException

    //Phase 11 Q9: PRNG for math.random/randomi. Bare mt19937 arithmetic
    //only — std::uniform_*_distribution is implementation-defined and
    //would break cross-platform determinism after math.srand. Reselected
    //from random_device at each Execute() so runs differ unless the
    //program calls math.srand itself.
    std::mt19937 m_rng;
    //K8: serializes the shared engine's draws and reseeds (host-boundary
    //leaf mutex — the engine is not a store).
    mutable std::mutex m_rngMutex;

    //Phase 9d: allocate a built-in Exception instance of the given class,
    //set message + populate backtrace from the current evaluation-stack snapshot,
    //and throw NLangThrow carrying its heap idx. The throw site is expected
    //to be a converted `throw std::runtime_error(...)` site; the caller
    //supplies the human-readable message (used for what() / debugging).
    [[noreturn]] void RaiseNlangException(int16_t classIdx, const std::string& msg);
    //K2 slice 4: building half of the above (no throw) — the NativeHost
    //raiseException callback roots the fresh object before throwing it
    //across the blocking-native GIL gap.
    int32_t BuildNlangException(int16_t classIdx, const std::string& msg);

    //v0.8.5: the message an NLangThrow carries for a thrown exception
    //object. cell[1] holds the message string handle (the ctor intrinsic
    //and RaiseNlangException share the layout); an empty message falls
    //back to the class leaf name (a bare `throw Exception()`-shaped
    //object still names itself). Anything unreadable (non-class object,
    //stale handle) keeps the caller's legacy text.
    std::string ThrownMessageOf(int32_t heapIdx, const char* fallback) const;

    //Phase 11: argument/range/parse errors of stdlib intrinsics raise the
    //BASE Exception (no dedicated argument-exception subclass exists).
    //Defined in IntrinsicsString.cpp; users are that family TU and the
    //public host bridge wrapper RaiseHostException.
    [[noreturn]] void RaiseNlangExceptionBase(const std::string& msg);

    //Phase 9d: returns true if the heap object at heapIdx is an instance of
    //the given target class or any of its subclasses. Walks the superClassIdx
    //chain (same pattern as OP_CheckCast). Used by the try/catch handler
    //lookup to decide whether a catch clause can handle the thrown exception.
    bool IsInstanceOrSubclass(int32_t heapIdx, uint16_t targetClassIdx);

    //Frames captured during exception unwinding (records live on the
    //carrier slot via UnwindFrames(); the UnwindFrame struct itself
    //moved to CallFrame.h at namespace scope in K7). Populated by
    //RecordUnwindAndPopFrame as RunFrames unwinds frames innermost-first;
    //cleared when a catch handler takes the exception, so only the
    //uncaught path yields observable text (FormatBacktrace renders it).

    //ByteStream side table: handle (1-based) → state.
    struct ByteStreamState {
        std::vector<uint8_t> buf;
        size_t pos = 0;
        bool closed = false;
        //Phase 8c — per-stream object-identity table for class-typed struct
        //fields. Two maps because serialize looks up heapIdx→id and deserialize
        //looks up id→heapIdx; never both directions in one operation. A stream
        //is either reading or writing within a session, so a single nextObjId
        //counter serves both maps; Reset (BS only) and Close clear all three.
        std::unordered_map<int32_t, uint32_t> serializeObjIds;   // heapIdx -> assignedId
        std::unordered_map<uint32_t, int32_t> deserializeObjIds; // assignedId -> heapIdx
        uint32_t nextObjId = 1;
    };
    std::vector<std::unique_ptr<ByteStreamState>> m_byteStreams;
    std::vector<int32_t> m_byteStreamFreeList;

    //FileStream side table: handle (1-based) → state.
    struct FileStreamState {
        std::unique_ptr<std::fstream> fs;
        bool writable = false;
        bool readable = false;
        bool closed = false;
        //Phase 8c — see ByteStreamState for rationale.
        std::unordered_map<int32_t, uint32_t> serializeObjIds;   // heapIdx -> assignedId
        std::unordered_map<uint32_t, int32_t> deserializeObjIds; // assignedId -> heapIdx
        uint32_t nextObjId = 1;
    };
    std::vector<std::unique_ptr<FileStreamState>> m_fileStreams;
    std::vector<int32_t> m_fileStreamFreeList;

    //Phase 8e-3: List<T> side table. All elements are heap idxs (boxed
    //primitives via OP_Box, or class refs directly). One CompiledClass named
    //"List" is shared by all instantiations (erasure). The synthetic
    //SnClassDecl "List<int>" / "List<Point>" / ... exists only at compile time.
    struct ListSlot {
        std::vector<int32_t> elements;  //each entry is a heap idx (RTK_Boxed or RTK_Class)
    };
    std::vector<ListSlot> m_listStore;       //index = handle-1 (0 reserved for null)
    std::vector<int32_t>  m_listFreeList;    //recycled slots after GC sweep
    int16_t m_listClassIdx = -1;             //set when "List" CompiledClass is located
    //Shared INTR_List_IndexOf/Contains matcher (was ~25 verbatim-
    //duplicated lines in each intrinsic): decodes the probe value and
    //compares it against each stored element — boxed primitives by tag
    //(strings by content, others by bits), reference values by identity.
    //Returns the matching index or -1.
    int32_t FindListElement(const ListSlot& list, int32_t value);
    //K8 formatting snapshots: one Container-domain section, copy out.
    std::vector<int32_t> SnapshotListElements(int32_t handle) const;
    std::vector<std::pair<int32_t, int32_t>> SnapshotDictEntries(
        int32_t handle) const;

    //Phase 8e-4: Dict<K,V> side table. Entries are (K heap idx, V heap idx)
    //pairs; K and V are uniformly heap idxs (boxed primitives via OP_Box at
    //the call site, or class refs passed directly). Linear-scan lookup is
    //O(n) per op — acceptable for typical NLang scripts; future optimization
    //(open-addressing hashtable) is a separate phase.
    struct DictSlot {
        std::vector<std::pair<int32_t, int32_t>> entries;
    };
    std::vector<DictSlot> m_dictStore;       //index = handle-1 (0 reserved for null)
    std::vector<int32_t>  m_dictFreeList;    //recycled slots after GC sweep
    int16_t m_dictClassIdx = -1;             //set when "Dict" CompiledClass is located

    //K1 Slice 8 / K8: Mutex side table. ownerId -1 = unlocked (context
    //ids are ≥ 0 — main IS 0, so 0 cannot double as the free sentinel).
    //Only context ids live here, never heap idxs, so GC does not trace
    //this store.
    //K8 two-phase park handshake: a parking lock registers kPending and
    //its recheck either commits (kCommitted — parked or stealing; an
    //unlock in the mark→steal stretch delivers through the K7
    //framesParked handshake) or finds kAcquired — unlock already
    //transferred ownership inside the registration→migration window
    //(the aborting parker returns holding the lock). Wakers stamp
    //kPending entries and deliver to kCommitted entries; exactly one
    //party consumes each entry: stamped → its aborting parker, committed
    //→ its delivering waker.
    struct SyncWaiter {   //P2-1: renamed from MutexWaiter — one waiter
                          //shape shared by the Mutex/Semaphore stores
        enum class Phase : uint8_t { kPending, kAcquired, kCommitted };
        int contextId;
        Phase phase = Phase::kPending;
    };
    struct MutexSlot {
        int ownerId = -1;
        std::deque<SyncWaiter> waiters;   //FIFO arrival order
    };
    std::vector<MutexSlot> m_mutexStore;     //index = handle-1 (0 reserved for null)
    std::vector<int32_t>   m_mutexFreeList;  //recycled slots after GC sweep
    int16_t m_mutexClassIdx = -1;            //set when "Mutex" CompiledClass is located

    //P2-1 (v0.10.17): Semaphore side table. count = available permits;
    //waiters are the FIFO parked acquirers (SyncWaiter, shared with the
    //Mutex store). Invariant: a live waiter implies count ==
    //0 — release hands the permit straight to the front waiter, it
    //never counts up past one.
    struct SemaphoreSlot {
        int count = 0;
        std::deque<SyncWaiter> waiters;   //FIFO arrival order
    };
    std::vector<SemaphoreSlot> m_semaphoreStore;   //index = handle-1 (0 reserved for null)
    std::vector<int32_t>       m_semaphoreFreeList;  //recycled slots after GC sweep
    int16_t m_semaphoreClassIdx = -1;         //set when "Semaphore" CompiledClass is located

    //K1 Slice 9 / K8: Channel side table. buffered holds element values
    //as heap idxs (boxed primitives or class refs) in FIFO order; live
    //(kPending/kCommitted) waiters exist only while the buffer is empty
    //(recv parks solely on an empty open channel; send hands off to the
    //front live waiter directly instead of buffering). GC traces
    //buffered through MarkChannelElements; waiters hold raw pointers,
    //never heap idxs.
    //K8 two-phase park handshake (same shape as SyncWaiter): kValue —
    //a value was written through resultPtr (send handoff or close
    //draining the buffer); kClosed — close chose the exception outcome
    //(the aborting parker raises it locally; the delivering waker arms
    //the committed shape through the wake call's rethrow parameter —
    //never a flag, which would race a mid-park abort).
    struct ChannelWaiter {
        enum class Phase : uint8_t {
            kPending, kValue, kClosed, kCommitted
        };
        int contextId;
        uint8_t* resultPtr;   //frame slot / evalArea band — park-stable
        Phase phase = Phase::kPending;
    };
    //Bounded sends park value+owner FIFO; send returns nothing, so
    //there is no result destination. Phase semantics as above: recv's
    //slot-free path moves the value into the buffer and stamps kValue
    //(the aborting sender's recheck returns with the send complete) or
    //delivers to kCommitted (moved + wake); close stamps kClosed /
    //collects the committed senders. No trace arm: a pending value
    //stays anchored by its sender's parked frame until it lands in
    //buffered (traced) or is dropped.
    struct PendingSend {
        enum class Phase : uint8_t {
            kPending, kValue, kClosed, kCommitted
        };
        int32_t value;
        int senderId;
        Phase phase = Phase::kPending;
    };
    struct ChannelSlot {
        std::deque<int32_t> buffered;
        std::deque<ChannelWaiter> waiters;
        bool closed = false;
        int32_t capacity = 0;   //K3 task 4: 0 = unbounded
        std::deque<PendingSend> pending;
    };
    std::vector<ChannelSlot> m_channelStore;   //index = handle-1 (0 reserved for null)
    std::vector<int32_t>     m_channelFreeList;//recycled slots after GC sweep
    int16_t m_channelClassIdx = -1;            //set when "Channel" CompiledClass is located

    //slot[0] of a container/sync-primitive instance heap entry holds
    //classIdx; slot[1] holds __handle (List/Dict/Mutex/Channel share the
    //layout). Used by GC trace/sweep and ReadListHandle.
    static constexpr int kListHandleFieldOffset = 1;
    //A RTK_Boxed heap record has layout [typeTag, valueLo, valueHi]
    //(0.7.5: 3 cells; the hi cell carries the upper half of 8-byte rows
    //and is ignored for narrower ones). Used by boxing/unboxing, GC
    //trace, and the shared BoxedValuesEqual comparator.
    static constexpr int kBoxedValueSlot = 1;
};

} // namespace nlang
