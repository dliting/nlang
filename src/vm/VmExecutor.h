#pragma once
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/NativeHost.h"
#include "BytecodeReader.h"
#include "IDebugHooks.h"
#include "IHostFunctions.h"
#include "IHostIo.h"
#include "TokenView.h"
#include "VmExecutorNativeHost.h"
#include "NativeLibraryLoader.h"
#include <cstdint>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
//Forward declaration: member signatures below take CallFrame by
//reference; the full definition sits with the data members.
    struct CallFrame;
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

    //Testing knobs (white-box GC pressure): clamp both thresholds so any
    //untraced handle turns stale almost immediately, and observe the live
    //string-object population for bounded-memory assertions.
    void SetGcStressThresholds(size_t records) {
        m_gcThreshold = records;
        m_strGcThreshold = records;
    }
    size_t LiveStringObjectCount() const;   //live slots, immortal included

    //IVmDebugView — definitions in VmExecutorDebug.cpp.
    size_t FrameCount() const override;
    DebugFrameInfo FrameInfo(size_t depth) const override;
    std::vector<DebugLocalValue> FrameLocals(size_t depth) const override;

    //Native functions (standard library AND third-party) share the public
    //NativeHost ABI (nlang/vm/NativeHost.h): each call receives a NativeHost
    //function table for all VM access, a return cell, the argument cells and
    //the declared parameter count. A namespace function's args start at
    //slot 0; methods pass the receiver in slot 0. Native code is stateless;
    //lookup is by the CompiledFunction name (a qualified "ns.name" for
    //library functions, a bare name for host-registered natives).
    using NativeFn = ::NativeFn;
    void RegisterNative(const std::string& name, NativeFn fn);

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

    void ExecuteFunction(const CompiledFunction& func,
        uint8_t* pResult, uint8_t* locals);

    //Host-bridge internals (definitions in VmExecutorHostBridge.cpp):
    //FillDefaults stages the Option-B defaults for a by-name call's
    //missing formals (what a compiler call site would have staged).
    void FillDefaults(const CompiledFunction& callee,
                      uint8_t* frameCells, uint32_t argc);
    void FillOneDefault(const CompiledFunction& callee,
                        const DefaultValueDesc& d, uint8_t* cell);

    //private（ResolveHostField 的描述符视图）：CompiledStruct/
    //CompiledClass 是无继承关系的两个类型，字段成员仅名字相同
    //（fieldCount/fieldNames/fieldTypeKinds，且后者同为
    //vector<uint16_t>，CompiledModule.h:148-161/:384-400）——视图持各
    //表指针，单一代码路径服务两表。
    struct HostFieldDesc {
        const std::vector<std::string>* fieldNames;
        const std::vector<uint16_t>* fieldTypeKinds;
        uint16_t fieldCount;
    };
    HostFieldDesc ResolveHostField(int32_t heapIdx, uint16_t fieldIndex,
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
    void OpCallFunc(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpMakeFunc(BytecodeReader& reader, uint8_t* pResult);
    void OpCallDelegate(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpCallDelegateOut(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpMakeBoundFunc(BytecodeReader& reader, uint8_t* pResult);
    void OpMakeVFunc(BytecodeReader& reader, uint8_t* pResult);
    void OpFuncEquality(BytecodeReader& reader, uint8_t* locals, OpCode op);
    void OpFunc_to_str(uint8_t* pResult);
    void OpCallFuncOut(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpCallMethodDirect(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpCallMethodDirectOut(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpCallMethod(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpCallIntrinsic(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult);
    void OpAllocStruct(BytecodeReader& reader, uint8_t* locals);
    void OpLoadField(BytecodeReader& reader, uint8_t* locals);
    void OpStoreField(BytecodeReader& reader, uint8_t* locals);
    void OpNew(BytecodeReader& reader, uint8_t* locals);
    void OpNullCheck(BytecodeReader& reader, uint8_t* locals);
    void OpCopyStruct(BytecodeReader& reader, uint8_t* locals);
    void OpAllocArray(BytecodeReader& reader, uint8_t* locals);
    void OpLoadElement(BytecodeReader& reader, uint8_t* locals);
    void OpStoreElement(BytecodeReader& reader, uint8_t* locals);
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
    //synthetic 4-byte this-only frame, intrinsic dispatch via
    //ExecuteIntrinsic, native records refused (no marshalling), plain
    //methods through ExecuteFunction. Returns the interned result
    //string (StrVal flattens a cons chain on first read).
    std::string CallToStringOverride(const CompiledFunction& callee,
        int32_t thisHeapIdx, int32_t classIdx);

    //Phase 8d — polymorphism check for class-typed deserialization.
    //Walks superClassIdx chain. Returns true if actualIdx is declaredIdx
    //or a subclass thereof.
    bool IsSubclassOf(uint16_t actualIdx, uint16_t declaredIdx);

    //Allocate an array on the heap.
    //Returns the heap index.
    int32_t AllocArrayOnHeap(uint16_t arrayTypeIdx, int32_t size);

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
    void MarkFuncReceiver(int32_t idx, std::vector<int32_t>& worklist);
    void MarkBoxedPayload(int32_t idx);
    void MarkClassFields(int32_t idx, std::vector<int32_t>& worklist);
    void MarkListElements(int32_t idx, std::vector<int32_t>& worklist);
    void MarkDictEntries(int32_t idx, std::vector<int32_t>& worklist);
    void MarkStructFields(int32_t idx, std::vector<int32_t>& worklist);
    void MarkArrayElements(int32_t idx, std::vector<int32_t>& worklist);
    void SweepPhase();
    void FreeOwnedStructs(int32_t heapIdx);
    void FreeNestedStructs(int32_t heapIdx, uint16_t structIdx);
    void FreeOwnedArrayStructElements(int32_t heapIdx);

    //Build a backtrace string from m_unwindFrames. Called when an exception
    //propagates out of main(); format is "  at <func> (<module>.n:<line>)".
    std::string FormatBacktrace() const;

    //Check if GC should run at a safepoint (function entry, loop back-edge).
    void CheckGCSafepoint();

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
    //bound handles).
    void ExecuteDelegateCall(const std::vector<int32_t>& handle,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
        uint32_t outMask);

    //Delegate-target resolution half of ExecuteDelegateCall: virtual
    //handles resolve by name on the receiver's runtime class (override
    //chain), static handles are a range-checked functions[] index.
    int ResolveDelegateTarget(const std::vector<int32_t>& handle);

    //Free-function delegate ABI (identical to OP_CallFunc, natives
    //included); the out write-back copies slots unshifted.
    void CallDelegateFree(const CompiledFunction& callee,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
        uint32_t outMask);

    //Bound-method delegate ABI: the captured receiver rides at callee
    //slot 0, caller args shift right by one; the out write-back reads
    //frame slot i+1 and stages back to callParamBase+i, reversing the
    //shift.
    void CallDelegateBound(const CompiledFunction& callee, int32_t thisIdx,
        uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
        uint32_t outMask);

    //Phase 9f: shared native-table dispatch for OP_CallFunc and the method
    //call paths (a native method receives `this` at args[0], mirroring the
    //bytecode calling convention). Throws when the host never registered
    //the name — failing at the call site rather than executing the
    //declaration's empty bytecode.
    void CallNative(const CompiledFunction& callee, uint16_t callParamBase,
        uint8_t* locals, uint8_t* pResult);

    //NativeHost ABI plumbing (definitions in VmExecutorNativeHost.cpp).
    void InitNativeHost(VmNativeHost& host);
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
    size_t m_recurseDepth = 0;
    const CompiledModule* m_currModule = nullptr;
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
    std::vector<StrObj> m_stringObjs;
    //Parallel to m_stringObjs — kept in step at exactly three sites:
    //AllocStringObj's growth arm, MarkPhase's head reset, SweepStrings'
    //tail reset.
    std::vector<bool> m_strMarkBits;
    std::vector<int32_t> m_strFreeList;
    std::vector<int32_t> m_constStrCache;  //constant idx -> immortal handle
    int32_t m_emptyStrHandle = 0;          //dedicated immortal ""
    //Name → host function table for native calls.
    std::unordered_map<std::string, NativeFn> m_natives;
    //Lazy on-demand loader for native modules (standard-library DLLs and
    //third-party modules use it). Created on first native-module need.
    std::unique_ptr<NativeLibraryLoader> m_upNativeLoader;

    //Last captured backtrace (filled by Execute's catch block).
    std::string m_lastBacktrace;

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
    int32_t AllocStringObj();                                //raw slot, sets m_gcPending
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
    std::vector<StructSlot> m_structHeap;

    //GC infrastructure.
    std::vector<uint8_t>  m_slotKinds;      //parallel to m_structHeap: RTK_Class/RTK_Struct/0=free
    std::vector<uint16_t> m_slotStructIdx;  //parallel to m_structHeap: struct CompiledStruct index (class uses slot[0])
    std::vector<bool>     m_markBits;       //parallel to m_structHeap: GC mark bit
    std::vector<int32_t>  m_freeList;       //free heap slot indices for reuse
    size_t m_gcThreshold = GC_THRESHOLD_DEFAULT;
    bool   m_gcPending = false;

    //String-store GC plumbing (collection activates in Task 2).
    static const size_t kStrGcThresholdDefault = 1024;
    size_t m_strGcThreshold = kStrGcThresholdDefault;
    static const size_t kShortStringMaxBytes = 40;  //Lua short-string cutoff
    std::unordered_map<std::string, int32_t> m_shortStrTable;  //weak: content -> live handle
    static constexpr uint8_t kStrFormDead = 0xFF;   //free-slot sentinel

    //Call frame stack for GC root set identification.
    struct CallFrame {
        uint8_t* locals;
        uint8_t* pResult;
        const CompiledFunction* func;
        uint16_t currentLine = 0;   //updated by OP_DebugInfo; 0 = unknown
        uint16_t currentPc = 0;     //ndb: pc of the current statement anchor
        //Phase 9d: per-frame stack of currently-caught exception heap idxs.
        //Pushed when a catch handler is entered, popped by OP_PopHandler at
        //catch-block exit. OP_Rethrow reads the top entry to re-raise. Per-
        //frame (not executor-global) so a cross-function call cannot leak a
        //parent's catch context into the callee.
        std::vector<int32_t> handlerExcStack;
    };
    std::vector<CallFrame> m_callStack;

    //Phase 9d: cached class indices for the built-in Exception hierarchy.
    //Initialized in Execute() via module.FindClass; -1 = not found (only
    //possible if the runtime is launched against a malformed module that
    //somehow lacks the built-in registrations).
    int16_t m_exceptionClassIdx = -1;
    int16_t m_nullPtrExcClassIdx = -1;
    int16_t m_divZeroExcClassIdx = -1;
    int16_t m_oobExcClassIdx = -1;
    int16_t m_assertExcClassIdx = -1;
    int16_t m_ioExcClassIdx = -1;       //Phase 11: IOException

    //Phase 11 Q9: PRNG for math.random/randomi. Bare mt19937 arithmetic
    //only — std::uniform_*_distribution is implementation-defined and
    //would break cross-platform determinism after math.srand. Reselected
    //from random_device at each Execute() so runs differ unless the
    //program calls math.srand itself.
    std::mt19937 m_rng;

    //Phase 9d: allocate a built-in Exception instance of the given class,
    //set message + populate backtrace from the current m_callStack snapshot,
    //and throw NLangThrow carrying its heap idx. The throw site is expected
    //to be a converted `throw std::runtime_error(...)` site; the caller
    //supplies the human-readable message (used for what() / debugging).
    [[noreturn]] void RaiseNlangException(int16_t classIdx, const std::string& msg);

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

    //Frames captured during exception unwinding. Populated by FrameGuard's
    //destructor when std::uncaught_exceptions() > 0. Ordered innermost-first
    //because innermost frame's destructor runs first during stack unwinding.
    struct UnwindFrame {
        std::string funcName;
        uint16_t currentLine;
    };
    std::vector<UnwindFrame> m_unwindFrames;

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

    //slot[0] of a List instance heap entry holds classIdx; slot[1] holds __handle.
    //Used by GC trace/sweep and ReadListHandle.
    static constexpr int kListHandleFieldOffset = 1;
    //A RTK_Boxed heap record has layout [typeTag, valueLo, valueHi]
    //(0.7.5: 3 cells; the hi cell carries the upper half of 8-byte rows
    //and is ignored for narrower ones). Used by boxing/unboxing, GC
    //trace, and the shared BoxedValuesEqual comparator.
    static constexpr int kBoxedValueSlot = 1;
};

} // namespace nlang
