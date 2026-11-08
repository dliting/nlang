/*---
    VmExecutorOpsArrayCheck.cpp — the array boundary-check opcode family
    (v0.12.7): the annotated-target rank check (Task 3) and the
    Object→array downcast type check (Task 5). Split from
    VmExecutorOpsArrayN.cpp when the family grew past that file's size
    budget; both handlers raise the catchable InvalidCastException.
---*/
#include "VmExecutor.h"
#include "GenericInstDescriptor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstring>
#include <stdexcept>

namespace nlang {

//v0.12.7 Task 3: boundary rank check — the runtime half of the promise
//model. An annotated target (rank >= 1) receiving an unannotated value
//(Array<E> spelling, or a value flowed through Object) verifies the
//record's rank. Null is a legal value of every array type (spec 4.1)
//and passes through unchanged; a non-array handle or a rank mismatch
//raises the catchable InvalidCastException.
void VmExecutor::OpCheckArrayRank(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    uint16_t expect = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + src, sizeof(heapIdx));
    if (heapIdx == 0) {
        std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
        return;
    }
    if (static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: OP_CheckArrayRank on invalid reference");
    //Views carry the same header prefix (rank at [2]) — the shared-prefix
    //layout is what makes this check shape-blind (v0.12.7 Task 8).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array
        && m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_ArrayView)
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array cast: value is not an array");
    const int32_t got = m_structHeap[static_cast<size_t>(heapIdx)][2];
    if (static_cast<uint16_t>(got) != expect)
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array rank mismatch: expected "
            + std::to_string(expect) + ", got " + std::to_string(got));
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
}

//v0.13.1 D5: the element-instantiation tail of a matching
//OP_CheckArrayType row — the target's descriptor spelling must equal
//the record's element stamp. A missing stamp means the element is not
//an instantiation and cannot satisfy an instantiation target.
void VmExecutor::CheckArrayTypeElemDescriptor(int32_t heapIdx,
    uint16_t descIdx, const uint8_t* locals) {
    const GenericInstDescriptor* target =
        CastTargetDescriptor(descIdx, locals);
    const GenericInstDescriptor* stamp = ArrayElemDescriptorOf(heapIdx);
    if (!stamp || stamp->Spelling() != target->Spelling())
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array element type mismatch: expected `"
            + target->Spelling() + "[]`, got `"
            + ArrayElemSpellingOf(heapIdx) + "[]`");
}

//v0.12.7 Task 5: the Object→array downcast check — `o as int[]`. The
//arrayTypes table dedups per merged image at link time (NcuLinker's
//MergeArrayTypes), so record type identity IS type identity: one
//comparison verifies rank and element type together. Null passes
//through; a non-array handle or a row mismatch raises the catchable
//InvalidCastException (the message names rank vs element type).
//v0.13.1 D5: an instantiation element target (`o as Box<int>[]`) adds
//the element stamp comparison after the row match — the erased row
//cannot tell Box<int>[] from Box<string>[].
void VmExecutor::OpCheckArrayType(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    uint16_t expectIdx = reader.ReadUint16();
    //v0.13.1: the element instantiation's descriptor row (0xFFFF = the
    //target's element is not an instantiation; the row check is whole).
    uint16_t descIdx = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + src, sizeof(heapIdx));
    if (heapIdx == 0) {
        std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
        return;
    }
    if (static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: OP_CheckArrayType on invalid reference");
    //Views: same accepted kind pair as OpCheckArrayRank above — the
    //typeIdx/rank cells this check reads sit at identical offsets on both
    //layouts (v0.12.7 Task 8).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array
        && m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_ArrayView)
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array cast: value is not an array");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const auto gotIdx = static_cast<uint16_t>(slot[1]);
    if (gotIdx == expectIdx) {
        if (descIdx != 0xFFFF)
            CheckArrayTypeElemDescriptor(heapIdx, descIdx, locals);
        std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
        return;
    }
    //Precise diagnostics: a rank mismatch names both ranks; same-rank
    //failures are element types.
    const auto& rows = m_currModule->arrayTypes;
    const auto gotRank = static_cast<uint8_t>(slot[2]);
    if (expectIdx < rows.size() && gotRank != rows[expectIdx].rank)
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array rank mismatch: expected "
            + std::to_string(rows[expectIdx].rank) + ", got "
            + std::to_string(static_cast<int>(gotRank)));
    RaiseNlangException(m_invalidCastExcClassIdx,
        "array element type mismatch");
}

//v0.13.1 D5: does the record's element type satisfy the Array<E> face
//target's type argument? Two tiers, because the erased arrayTypes row
//cannot subdivide: an instantiation argument requires the allocation-
//time element stamp (its spelling must equal the argument's canonical
//form); a leaf argument compares at row granularity — scalar kinds by
//RTK (enum elements erase to int32 by design: one row, one verdict),
//struct/class elements by link-key spelling (the row carries the
//erased row index and the table keys ARE the link keys). Array-typed
//arguments keep kind granularity (nested-array rows do not record
//their element row — a pre-existing row-granularity limit, unchanged).
bool VmExecutor::ArrayElemMatchesTarget(int32_t heapIdx,
                                        const GenericInstDescriptor& target) {
    const auto& typeArgs = target.TypeArgs();
    //Defensive: an unshaped target (or a multi-argument face, which
    //Array never is) degrades to the row check that already passed.
    if (typeArgs.size() != 1)
        return true;
    const TypeArgDesc& arg = typeArgs[0];
    const uint16_t rowIdx = static_cast<uint16_t>(
        m_structHeap[static_cast<size_t>(heapIdx)][1]);
    if (rowIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: array record with invalid type row");
    const CompiledArrayType& row = m_currModule->arrayTypes[rowIdx];
    const GenericInstDescriptor* stamp = ArrayElemDescriptorOf(heapIdx);
    if (!arg.nested.empty())
        //An instantiation element: only the stamp can prove it.
        return stamp && stamp->Spelling() == CanonicalArgSpelling(arg);
    if (row.elemKind != arg.resolvedKind)
        return false;
    if (row.elemKind == RTK_Struct)
        return row.elemTypeIdx < m_currModule->structs.size()
            && m_currModule->structs[row.elemTypeIdx].name == arg.spelling;
    if (row.elemKind == RTK_Class)
        //A stamped record's element IS an instantiation — a leaf class
        //argument never matches it (and a stampless template-row
        //element cannot arise: bare template names are not legal
        //element types).
        return !stamp
            && row.elemTypeIdx < m_currModule->classes.size()
            && m_currModule->classes[row.elemTypeIdx].name == arg.spelling;
    return true;  //scalars/string/array-of leaves: RTK granularity
}

//v0.13.1 D5: the record's element type spelling for cast diagnostics —
//the stamped instantiation's spelling when present, else the row's
//identity (registry scalar names; struct/class table keys; array
//elements degrade to "array" — nested rows do not record their element).
std::string VmExecutor::ArrayElemSpellingOf(int32_t heapIdx) {
    if (const GenericInstDescriptor* stamp = ArrayElemDescriptorOf(heapIdx))
        return stamp->Spelling();
    const uint16_t rowIdx = static_cast<uint16_t>(
        m_structHeap[static_cast<size_t>(heapIdx)][1]);
    if (rowIdx >= m_currModule->arrayTypes.size())
        return "?";
    const CompiledArrayType& row = m_currModule->arrayTypes[rowIdx];
    if (row.elemKind == RTK_String)
        return "string";
    if (int pi = ScalarPrimIndexOfRtk(row.elemKind); pi >= 0)
        return kScalarPrims[pi].name;
    if (row.elemKind == RTK_Struct
        && row.elemTypeIdx < m_currModule->structs.size())
        return m_currModule->structs[row.elemTypeIdx].name;
    if (row.elemKind == RTK_Class
        && row.elemTypeIdx < m_currModule->classes.size())
        return m_currModule->classes[row.elemTypeIdx].name;
    if (row.elemKind == RTK_Array)
        return "array";
    return "?";
}

} // namespace nlang
