#pragma once
#include "CompiledModule.h"
#include "BytecodeReader.h"
#include <cstdint>
#include <fstream>
#include <memory>
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

class VmExecutor {
public:
    VmExecutor() = default;
    ~VmExecutor();

    int Execute(const CompiledModule& module);

    //Backtrace captured from the last Execute() call. Empty if execution
    //succeeded without throwing.
    const std::string& Backtrace() const { return m_lastBacktrace; }

private:
    void ExecuteFunction(const CompiledFunction& func,
        uint8_t* pResult, uint8_t* locals);

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
    std::string FormatArray(int32_t heapIdx, int depth);
    std::string FormatList(int32_t handle, int depth);
    std::string FormatDict(int32_t handle, int depth);
    //Invoke virtual toString on a class instance by heap idx, returning
    //the result string. Mirrors OP_CallMethod's vtable walk; used by
    //collection formatters for class-typed elements.
    std::string InvokeVirtualToString(int32_t thisHeapIdx);

    //Phase 8d — polymorphism check for class-typed deserialization.
    //Walks superClassIdx chain. Returns true if actualIdx is declaredIdx
    //or a subclass thereof.
    bool IsSubclassOf(uint16_t actualIdx, uint16_t declaredIdx);

    //Allocate an array on the heap.
    //Returns the heap index.
    int32_t AllocArrayOnHeap(uint16_t arrayTypeIdx, int32_t size);

    //GC: mark-sweep garbage collection.
    //Design decisions (see docs/vm-architecture.md for full rationale):
    //  1. Safepoint-triggered, not allocation-point-triggered.
    //     Why: avoids tracking tempSlot/tempSlot2 references in MarkPhase.
    //  2. Precise scan via LocalDescriptor, not conservative byte scan.
    //     Why: decouples GC from stack frame physical layout (VALUE_SIZE, alignment).
    //  3. m_slotStructIdx parallel array for struct type identification.
    //     Why: struct objects have no type header; smaller change than adding one.
    //  4. Iterative mark with worklist, not recursive.
    //     Why: avoids stack overflow on deep object chains (e.g. linked lists).
    void CollectGarbage();
    void MarkPhase();
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
    //  RTK_Boxed + tag RTK_Int32  → value-bit equality
    //  RTK_Boxed + tag RTK_Float  → IEEE 754 value-bit equality (NaN≠NaN)
    //  RTK_Boxed + tag RTK_String → string-pool content equality
    //Returns false when either idx is out of bounds or kind mismatch.
    bool DictKeysEqual(int32_t k1, int32_t k2) const;

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
    std::vector<std::string> m_stringPool;

    //Last captured backtrace (filled by Execute's catch block).
    std::string m_lastBacktrace;

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

    //Call frame stack for GC root set identification.
    struct CallFrame {
        uint8_t* locals;
        uint8_t* pResult;
        const CompiledFunction* func;
        uint16_t currentLine = 0;   //updated by OP_DebugInfo; 0 = unknown
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

    //Phase 9d: allocate a built-in Exception instance of the given class,
    //set message + populate backtrace from the current m_callStack snapshot,
    //and throw NLangThrow carrying its heap idx. The throw site is expected
    //to be a converted `throw std::runtime_error(...)` site; the caller
    //supplies the human-readable message (used for what() / debugging).
    [[noreturn]] void RaiseNlangException(int16_t classIdx, const std::string& msg);

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
    //A RTK_Boxed heap slot has layout [typeTag, valueBits]. Used by List
    //IndexOf/Contains to compare primitive elements by value (C2 fix).
    static constexpr int kBoxedValueSlot = 1;
};

} // namespace nlang
