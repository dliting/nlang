/*---
    FieldOffsets.cpp — 编译期字段偏移求解：struct/class 数据字段在堆记录内的字节偏移，
    是运行期 VmClassLayout 布局视图的编译期镜像（自有字段表沿超类链 base-first 拼接）。
    从 EmitExprMember.cpp 抽取（0.9.12 源尺寸守卫拆分，一职责一 TU，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/runtime/NodeConsts.h>
#include <vector>

namespace nlang {

//Heap field addressing (0.7.5 Task 6 Step A2): every struct/class data
//field occupies a uniform 2-cell slot (kHeapFieldStrideBytes = 8) so
//64-bit scalars fit without per-field width logic. Struct field i sits
//at cell 2i; class data starts after the classIdx cell (field i at
//cell 1+2i). Compile-time offsets here must match runtime
//AllocStructOnHeap/AllocClassOnHeap in VmExecutor.
int VmBackend::FindFieldOffset(SnStructDecl& structDecl, const std::string& fieldName) {
    uint16_t off = 0;
    for (auto& sf : structDecl.Fields()) {
        if (sf.Name() == fieldName)
            return off;
        off += kHeapFieldStrideBytes;
    }
    return -1;
}

//Returns class field offset in bytes (including +4 for classIdx slot), or -1.
//Object layout: [classIdx, root_ancestor_fields..., parent_fields..., own_fields...]
//Cell[0] = classIdx (1 cell, used for runtime virtual dispatch via OP_CallMethod).
//Data fields occupy cells 1, 3, 5, ... (2 cells each).
int VmBackend::FindClassFieldOffset(SnClassDecl& classDecl, const std::string& fieldName) {
    //Phase 9d: built-in Exception classes expose message/backtrace fields
    //that aren't materialized as SnClassDecl members (the synthetic class
    //decl has empty Members()). Runtime layout (VmBackend::RegisterBuiltinClasses):
    //  cell[1] = message   → offset kHeapCellBytes (4)
    //  cell[3] = backtrace → offset kHeapCellBytes + kHeapFieldStrideBytes (12)
    //Walk SuperClass chain so user subclasses of Exception also resolve.
    if (classDecl.IsBuiltinClass()
        && IsBuiltinExceptionName(classDecl.Name())) {
        if (fieldName == "message")  return kHeapCellBytes;
        if (fieldName == "backtrace") return kHeapCellBytes + kHeapFieldStrideBytes;
        return -1;
    }
    //Collect ancestor chain from root to direct parent
    std::vector<SnClassDecl*> ancestors;
    auto* pSuper = classDecl.SuperClass();
    while (pSuper) {
        ancestors.push_back(pSuper);
        pSuper = pSuper->SuperClass();
    }
    //Search from root ancestor to direct parent (reversed order)
    uint16_t off = kHeapCellBytes; //skip the classIdx cell
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        int found = ClassAncestorFieldOffset(**it, fieldName, off);
        if (found >= 0)
            return found;
    }
    //Search in this class (after all parent fields)
    for (auto& member : classDecl.Members()) {
        if (member.Kind() == NK_ClassField && member.Name() == fieldName)
            return off;
        if (member.Kind() == NK_ClassField)
            off += kHeapFieldStrideBytes;
    }
    return -1;
}

//FindClassFieldOffset arm: one ancestor in the root→parent walk. Returns
//the field offset when the field lives in this ancestor, else -1; `off`
//always advances past the ancestor's data fields.
int VmBackend::ClassAncestorFieldOffset(SnClassDecl& ancestor,
                                        const std::string& fieldName,
                                        uint16_t& off) {
    //Phase 9d: the built-in Exception row contributes 2 runtime fields
    //(message, backtrace) that aren't in the synthetic Members() list —
    //the Exception base record carries them (BuiltinClasses.cpp), so
    //account for them here. 0.9.12 (v2.7): only the Exception row owns
    //the pair; intermediate built-in exception subclasses (IOException,
    //…) own no fields and contribute nothing, because the runtime
    //layout view joins own-only lists. Pre-v2.7 the registration side
    //flattened the pair into every subclass record and this walk added
    //2 fields per matching ancestor; with own-only records counting
    //twice would push every field below Exception past the allocation.
    if (ancestor.IsBuiltinClass()
        && IsBuiltinExceptionName(ancestor.Name())) {
        if (ancestor.Name() == "Exception") {
            if (fieldName == "message")  return off;
            if (fieldName == "backtrace") return off + kHeapFieldStrideBytes;
            off += 2 * kHeapFieldStrideBytes;
        }
        return -1;
    }
    for (auto& member : ancestor.Members()) {
        if (member.Kind() == NK_ClassField && member.Name() == fieldName)
            return off;
        if (member.Kind() == NK_ClassField)
            off += kHeapFieldStrideBytes;
    }
    return -1;
}

} //namespace nlang
