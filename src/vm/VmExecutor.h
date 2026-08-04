#pragma once
#include "CompiledModule.h"
#include "BytecodeReader.h"
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang {

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
    //Recursive over nested struct fields. Throws on class/array fields
    //(Phase 8c concern) and on depth-limit overflow.
    //Writer callable signature: void(const uint8_t* p, size_t n)
    template<typename Writer>
    void SerializeStructFields(int32_t heapIdx, uint16_t structIdx,
        Writer&& write, int depth = 0);

    //Deserialize fields from a byte source into a freshly-allocated struct.
    //Reader callable signature: void(uint8_t* dst, size_t n) — must throw
    //on short read (EOF). Caller must have pre-allocated root struct via
    //AllocStructOnHeap; nested struct slots are allocated here.
    template<typename Reader>
    void DeserializeStructFields(int32_t heapIdx, uint16_t structIdx,
        Reader&& read, int depth = 0);

    //Allocate a struct on the heap with recursive nested struct allocation.
    //Returns the heap index.
    int32_t AllocStructOnHeap(uint16_t structIdx);

    //Allocate a class object on the heap with recursive field allocation.
    //Returns the heap index.
    int32_t AllocClassOnHeap(uint16_t classIdx);

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

    static const size_t RECURSE_LIMIT = 1000;
    static const size_t GC_THRESHOLD_DEFAULT = 1024;
    static const size_t STRUCT_SERIALIZE_DEPTH_LIMIT = 64;
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
    };
    std::vector<CallFrame> m_callStack;

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
    };
    std::vector<std::unique_ptr<ByteStreamState>> m_byteStreams;
    std::vector<int32_t> m_byteStreamFreeList;

    //FileStream side table: handle (1-based) → state.
    struct FileStreamState {
        std::unique_ptr<std::fstream> fs;
        bool writable = false;
        bool readable = false;
        bool closed = false;
    };
    std::vector<std::unique_ptr<FileStreamState>> m_fileStreams;
    std::vector<int32_t> m_fileStreamFreeList;
};

} // namespace nlang
