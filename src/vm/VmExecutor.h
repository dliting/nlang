#pragma once
#include "CompiledModule.h"
#include "BytecodeReader.h"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang {

class VmExecutor {
public:
    int Execute(const CompiledModule& module);

private:
    void ExecuteFunction(const CompiledFunction& func,
        uint8_t* pResult, uint8_t* locals);

    //Deep-copy a struct from srcHeapIdx to a new heap slot.
    //Returns the new heap index.
    int32_t DeepCopyStruct(int32_t srcHeapIdx, uint16_t structIdx);

    //Allocate a struct on the heap with recursive nested struct allocation.
    //Returns the heap index.
    int32_t AllocStructOnHeap(uint16_t structIdx);

    //Allocate a class object on the heap with recursive field allocation.
    //Returns the heap index.
    int32_t AllocClassOnHeap(uint16_t classIdx);

    //GC: mark-sweep garbage collection.
    //Design decisions (see docs/superpowers/specs/ phase 5 plan):
    //  1. Safepoint-triggered, not allocation-point-triggered.
    //     Why: avoids tracking tempSlot/tempSlot2 references in MarkPhase.
    //  2. Precise scan via LocalDescriptor, not conservative byte scan.
    //     Why: decouples GC from stack frame physical layout (VALUE_SIZE, alignment).
    //  3. m_slotStructIdx parallel array for struct type identification.
    //     Why: struct objects have no type header; smaller change than adding one.
    void CollectGarbage();
    void MarkPhase();
    void MarkObject(int32_t heapIdx);
    void MarkStruct(int32_t heapIdx);
    void SweepPhase();
    void FreeOwnedStructs(int32_t heapIdx);
    void FreeNestedStructs(int32_t heapIdx, uint16_t structIdx);

    //Check if GC should run at a safepoint (function entry, loop back-edge).
    void CheckGCSafepoint();

    static const size_t RECURSE_LIMIT = 1000;
    static const size_t GC_THRESHOLD_DEFAULT = 1024;
    size_t m_recurseDepth = 0;
    const CompiledModule* m_currModule = nullptr;
    std::vector<std::string> m_stringPool;

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
        uint16_t localsSize;
        uint8_t* pResult;
        const CompiledFunction* func;
    };
    std::vector<CallFrame> m_callStack;
};

} // namespace nlang
