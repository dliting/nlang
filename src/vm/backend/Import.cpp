/*---
    Import.cpp — 跨模块导入合并与字节码重映射
    （InstructionStride / RemapBytecode / Phase A 合并 / Phase B 终化）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnData.h>
#include <cassert>

namespace nlang {

//Stride of the no-operand opcode family; 0 when op is not in it.
static size_t NoOperandStride(OpCode op) {
    switch (op) {
        case OpCode::OP_Return:
        case OpCode::OP_Stop:
        case OpCode::OP_ConstZero:
        case OpCode::OP_Array_to_str:
        case OpCode::OP_Func_to_str:
        case OpCode::OP_ParaEnd:
        case OpCode::OP_Rethrow:
        case OpCode::OP_PopHandler:
            return 1;  // no operands
        default:
            return 0;
    }
}

//Stride of the one-uint16-operand opcode family; 0 when op is not in it.
static size_t SingleU16OperandStride(OpCode op) {
    switch (op) {
        case OpCode::OP_ConstString:
        case OpCode::OP_AssertFail:
        case OpCode::OP_VarLocal:
        case OpCode::OP_Assign:
        case OpCode::OP_Enum_to_str:
        case OpCode::OP_LogicalNot:
        case OpCode::OP_Switch:
        case OpCode::OP_DebugInfo:
        case OpCode::OP_NullCheck:
        case OpCode::OP_CheckCast:
        case OpCode::OP_Throw:
        case OpCode::OP_MakeFunc:
        case OpCode::OP_MakeBoundFunc:
        case OpCode::OP_MakeVFunc:
            return 1 + 2;  // one uint16 operand
        default:
            return 0;
    }
}

//Stride of the two-uint16-operand opcode family; 0 when op is not in it.
static size_t DoubleU16OperandStride(OpCode op) {
    switch (op) {
        case OpCode::OP_JumpIfNot:
        case OpCode::OP_Concat_str:
        case OpCode::OP_Eq_str:
        case OpCode::OP_Ne_str:
        case OpCode::OP_Less_str:
        case OpCode::OP_LessEqual_str:
        case OpCode::OP_Greater_str:
        case OpCode::OP_GreaterEqual_str:
        case OpCode::OP_StrLen:
        case OpCode::OP_CallFunc:
        case OpCode::OP_CallMethod:
        case OpCode::OP_CallMethodDirect:
        case OpCode::OP_CallIntrinsic:
        case OpCode::OP_CallDelegate:
        case OpCode::OP_Eq_func:
        case OpCode::OP_Ne_func:
        case OpCode::OP_New:
        case OpCode::OP_ArrayLength:
            return 1 + 2 + 2;  // two uint16 operands
        default:
            return 0;
    }
}

//Phase 9c cross-module: returns the total byte size of an instruction
//(opcode + all operands). Used by RemapBytecode to walk a bytecode buffer.
//All operands are uint16 (2 bytes) except OP_Box/OP_Unbox (1-byte tag),
//OP_ConstInt32/OP_ConstFloat/OP_Jump (4 / 4 / 2-byte immediate), and
//OP_JumpIfNot (2 + 2 = 4). The 0.7.5 kind-immediate families (Add..Mod,
//Neg, Cmp, PrimCast, Prim_to_str, ConstInt64/ConstDouble) carry uint8
//kind immediates and/or 8-byte immediates — see the arms below. The 11
//remap-relevant opcodes are tagged in the second switch below.
static size_t InstructionStride(OpCode op) {
    if (size_t stride = NoOperandStride(op))
        return stride;
    if (size_t stride = SingleU16OperandStride(op))
        return stride;
    if (size_t stride = DoubleU16OperandStride(op))
        return stride;
    switch (op) {
        case OpCode::OP_Prim_to_str:   // uint8 kind
        case OpCode::OP_Box:
        case OpCode::OP_Unbox:         // uint8 tag
            return 1 + 1;
        case OpCode::OP_PrimCast:
            return 1 + 1 + 1;  // uint8 srcKind + uint8 dstKind
        case OpCode::OP_Neg:
            return 1 + 1 + 2;  // uint8 kind + uint16 dst
        case OpCode::OP_Cmp:
            return 1 + 1 + 1 + 2 + 2;  // kind + cmpOp + two uint16 slots
        case OpCode::OP_Add: case OpCode::OP_Sub:
        case OpCode::OP_Mul: case OpCode::OP_Div:
        case OpCode::OP_Mod:
            return 1 + 1 + 2 + 2;  // uint8 kind + two uint16 slots
        case OpCode::OP_Jump: case OpCode::OP_Case:
            return 1 + 2;  // int16 / uint16
        case OpCode::OP_ConstInt32: case OpCode::OP_ConstFloat:
            return 1 + 4;
        case OpCode::OP_ConstInt64: case OpCode::OP_ConstDouble:
            return 1 + 8;  // 8-byte immediate (0.7.5)
        case OpCode::OP_CallFuncOut: case OpCode::OP_CallMethodDirectOut:
        case OpCode::OP_CallDelegateOut:
            return 1 + 2 + 2 + 4;  // uint16 + uint16 + uint32 outMask (Phase 9e / 13)
        case OpCode::OP_AllocStruct: case OpCode::OP_LoadField:
        case OpCode::OP_StoreField: case OpCode::OP_CopyStruct:
        case OpCode::OP_AllocArray: case OpCode::OP_LoadElement:
        case OpCode::OP_StoreElement: case OpCode::OP_StrByteAt:
            return 1 + 2 + 2 + 2;  // three uint16 operands
        case OpCode::OP_StrForeachStep:
            return 1 + 2 + 2 + 2 + 2;  // four uint16 operands (0.7.5)
        default:
            //Unknown opcode — should never happen. Returning 1 lets the
            //walker make progress (likely produces garbage but doesn't
            //loop forever); the merged module will fail at runtime.
            assert(false && "unknown opcode in RemapBytecode");
            return 1;
    }
}

//Patch the uint16 operand at byte offset `off` through remap table `m`.
//Indices absent from the table are left as-is (best effort).
static void PatchU16Operand(std::vector<uint8_t>& bc, size_t off,
    const std::unordered_map<uint32_t, uint32_t>& m) {
    uint16_t oldv = static_cast<uint16_t>(bc[off])
                  | (static_cast<uint16_t>(bc[off + 1]) << 8);
    auto it = m.find(oldv);
    if (it == m.end())
        return;  // not in the remap table — leave as-is (best effort)
    uint16_t newv = static_cast<uint16_t>(it->second);
    bc[off]     = static_cast<uint8_t>(newv & 0xFF);
    bc[off + 1] = static_cast<uint8_t>((newv >> 8) & 0xFF);
}

//Phase 9c cross-module: walk bytecode and patch the 11 cross-module-indexed
//operand kinds (see cross-module-import-infrastructure.md Layer 4 table).
//Other operands (local offsets, jump targets, intrinsic IDs, type tags,
//debug line numbers) are module-local and do not need remapping.
void VmBackend::RemapBytecode(std::vector<uint8_t>& bc, const PerModuleRemap& pm) {
    size_t pos = 0;
    while (pos < bc.size()) {
        OpCode op = static_cast<OpCode>(bc[pos]);
        switch (op) {
            case OpCode::OP_ConstString:
            case OpCode::OP_AssertFail:
            case OpCode::OP_CallMethod:
                PatchU16Operand(bc, pos + 1, pm.stringMap);
                break;
            case OpCode::OP_CallFunc:
            case OpCode::OP_CallMethodDirect:
            case OpCode::OP_CallFuncOut:
            case OpCode::OP_CallMethodDirectOut:
            case OpCode::OP_MakeFunc:
            case OpCode::OP_MakeBoundFunc:
                PatchU16Operand(bc, pos + 1, pm.functionMap);
                break;
            case OpCode::OP_MakeVFunc:
                PatchU16Operand(bc, pos + 1, pm.stringMap);
                break;
            case OpCode::OP_New:
            case OpCode::OP_CheckCast:
                PatchU16Operand(bc, pos + 1, pm.classMap);
                break;
            case OpCode::OP_AllocStruct:
                PatchU16Operand(bc, pos + 1, pm.structMap);
                break;
            case OpCode::OP_CopyStruct:
                // dst, src, structIdx — third operand
                PatchU16Operand(bc, pos + 5, pm.structMap);
                break;
            case OpCode::OP_AllocArray:
                PatchU16Operand(bc, pos + 1, pm.arrayTypeMap);
                break;
            case OpCode::OP_Enum_to_str:
                PatchU16Operand(bc, pos + 1, pm.enumMap);
                break;
            default:
                break;
        }
        pos += InstructionStride(op);
    }
}

//Phase 9c cross-module Phase A.
//Push imported strings/classes/structs/arrayTypes into m_compiledModule,
//build per-module remap tables (stringMap/classMap/structMap/arrayTypeMap),
//and apply partial metadata remap (superClassIdx + fieldClassIndices +
//fieldStructIndices for classes; fieldClassIndices + fieldStructIndices
//for structs; elemTypeIdx for arrayTypes). methodIndices/constructorIdx
//are deferred to Phase B (needs functionMap).
void VmBackend::MergeImportedClassesStructsArrays() {
    m_importRemaps.clear();
    m_importRemaps.reserve(m_importedModules.size());
    MergeImportedTypeTables();
    RemapImportedTypeMetadata();
}

//Stage A.1: per-module build stringMap + classMap/structMap/arrayTypeMap
//by pushing (deduped) entries into m_compiledModule.
void VmBackend::MergeImportedTypeTables() {
    for (auto& im : m_importedModules) {
        PerModuleRemap pm;

        for (uint32_t i = 0; i < im.stringConstants.size(); ++i)
            pm.stringMap[i] = AddStringConstant(im.stringConstants[i]);

        for (uint32_t i = 0; i < im.classes.size(); ++i) {
            int existing = m_compiledModule.FindClass(im.classes[i].name);
            if (existing >= 0) {
                pm.classMap[i] = static_cast<uint32_t>(existing);
                continue;  // dedup to existing (e.g. user Object / built-in)
            }
            pm.classMap[i] = m_compiledModule.classes.size();
            m_compiledModule.classes.push_back(im.classes[i]);
            pm.classWasPushed.insert(i);
        }

        for (uint32_t i = 0; i < im.structs.size(); ++i) {
            int existing = m_compiledModule.FindStruct(im.structs[i].name);
            if (existing >= 0) {
                pm.structMap[i] = static_cast<uint32_t>(existing);
                continue;
            }
            pm.structMap[i] = m_compiledModule.structs.size();
            m_compiledModule.structs.push_back(im.structs[i]);
            //Push empty typeNames to keep m_structFieldTypeNames parallel
            //with m_compiledModule.structs. ResolveStructClassRefs' inner
            //loop iterates typeNames[i].size() so empty → no-op. The
            //v1.12 m_structFieldTypes parallel list follows the same
            //discipline (merged structs keep their copied descriptors).
            m_structFieldTypeNames.push_back({});
            m_structFieldTypes.push_back({});
            pm.structWasPushed.insert(i);
        }

        for (uint32_t i = 0; i < im.arrayTypes.size(); ++i) {
            //Push without dedup — RegisterArrayTypes' FindArray will dedup
            //when user code references the same type. Multiple identical
            //entries here are harmless (just slightly wasteful).
            pm.arrayTypeMap[i] = m_compiledModule.arrayTypes.size();
            m_compiledModule.arrayTypes.push_back(im.arrayTypes[i]);
        }

        m_importRemaps.push_back(std::move(pm));
    }
}

//Stage A.2: partial metadata remap on pushed entries only (R8-1).
void VmBackend::RemapImportedTypeMetadata() {
    for (size_t m = 0; m < m_importedModules.size(); ++m) {
        auto& im = m_importedModules[m];
        auto& pm = m_importRemaps[m];
        RemapImportedClassMetadata(im, pm);
        RemapImportedStructMetadata(im, pm);
        RemapImportedArrayTypeMetadata(im, pm);
    }
}

void VmBackend::RemapImportedClassMetadata(CompiledModule& im,
                                           PerModuleRemap& pm) {
    for (uint32_t i = 0; i < im.classes.size(); ++i) {
        if (pm.classWasPushed.find(i) == pm.classWasPushed.end()) continue;
        uint32_t targetIdx = pm.classMap[i];
        auto& cc = m_compiledModule.classes[targetIdx];
        if (cc.superClassIdx >= 0) {
            auto it = pm.classMap.find(static_cast<uint32_t>(cc.superClassIdx));
            if (it != pm.classMap.end())
                cc.superClassIdx = static_cast<int16_t>(it->second);
        }
        for (auto& idx : cc.fieldStructIndices)
            if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.structMap.at(idx));
        for (auto& idx : cc.fieldClassIndices)
            if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.classMap.at(idx));
        //v1.12: field descriptors reference the producer's tables —
        //remap for the pushed copy (chained re-export re-serializes
        //them with our numbering).
        for (auto& td : cc.fieldTypeDescs)
            RemapTypeDesc(td, pm.structMap, pm.classMap);
    }
}

void VmBackend::RemapImportedStructMetadata(CompiledModule& im,
                                            PerModuleRemap& pm) {
    for (uint32_t i = 0; i < im.structs.size(); ++i) {
        if (pm.structWasPushed.find(i) == pm.structWasPushed.end()) continue;
        uint32_t targetIdx = pm.structMap[i];
        auto& cs = m_compiledModule.structs[targetIdx];
        for (auto& idx : cs.fieldStructIndices)
            if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.structMap.at(idx));
        for (auto& idx : cs.fieldClassIndices)
            if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.classMap.at(idx));
        //v1.12: same descriptor remap as the class loop above.
        for (auto& td : cs.fieldTypeDescs)
            RemapTypeDesc(td, pm.structMap, pm.classMap);
    }
}

void VmBackend::RemapImportedArrayTypeMetadata(CompiledModule& im,
                                               PerModuleRemap& pm) {
    for (uint32_t i = 0; i < im.arrayTypes.size(); ++i) {
        uint32_t targetIdx = pm.arrayTypeMap[i];
        auto& at = m_compiledModule.arrayTypes[targetIdx];
        if (at.elemTypeIdx == 0xFFFF) continue;
        if (at.elemKind == RTK_Struct)
            at.elemTypeIdx = static_cast<uint16_t>(pm.structMap.at(at.elemTypeIdx));
        else if (at.elemKind == RTK_Class)
            at.elemTypeIdx = static_cast<uint16_t>(pm.classMap.at(at.elemTypeIdx));
    }
}

//Phase 9c cross-module Phase B.
//Push imported enumNames + function placeholders, copy + remap bytecode,
//complete class metadata (methodIndices + constructorIdx), and fill
//m_funcIndexMap[stub] via m_importedFuncSourceIdx side-table.
void VmBackend::MergeImportedFinalize() {
    PushImportedEnumAndFunctionPlaceholders();
    CopyImportedFunctionBytecode();
    RemapImportedMethodIndices();
    BindImportedFunctionStubs();
}

//Stage B.1: build enumMap + functionMap by pushing placeholders.
void VmBackend::PushImportedEnumAndFunctionPlaceholders() {
    for (size_t m = 0; m < m_importedModules.size(); ++m) {
        auto& im = m_importedModules[m];
        auto& pm = m_importRemaps[m];

        for (uint32_t i = 0; i < im.enumNames.size(); ++i) {
            pm.enumMap[i] = m_compiledModule.enumNames.size();
            m_compiledModule.enumNames.push_back(im.enumNames[i]);
        }

        for (uint32_t i = 0; i < im.functions.size(); ++i)
            PushImportedFunctionPlaceholder(im, pm, i);
    }
}

//Push one imported function record as a placeholder, remapping its v1.12
//type descriptors into our table numbering. Bytecode is filled in stage B.2.
void VmBackend::PushImportedFunctionPlaceholder(CompiledModule& im,
                                                PerModuleRemap& pm,
                                                uint32_t i) {
    pm.functionMap[i] = m_compiledModule.functions.size();
    CompiledFunction placeholder;
    placeholder.name = im.functions[i].name;
    placeholder.paramCount = im.functions[i].paramCount;
    placeholder.localsSize = im.functions[i].localsSize;
    placeholder.returnTypeKind = im.functions[i].returnTypeKind;
    placeholder.intrinsicId = im.functions[i].intrinsicId;
    //Phase 9f: native flag must survive the merge — the producer
    //wrote no bytecode for a native declaration, so a dropped flag
    //would leave the consumer calling empty bytecode (silent stale
    //pResult instead of a native table lookup).
    placeholder.isNative = im.functions[i].isNative;
    //v1.9 (debugger): locals + source file must survive the
    //merge. locals absence was a pre-existing GC root-set hole:
    //MarkPhase walks func->locals of every frame, so imported
    //frames had an empty root set and live objects could be
    //swept; the debugger also needs them for `info locals`.
    placeholder.locals = im.functions[i].locals;
    placeholder.sourceFile = im.functions[i].sourceFile;
    //v1.12: type descriptors must survive the merge for chained
    //re-export (a consumer saving its own .nmod re-serializes
    //these placeholders) — remap their table indices into ours.
    placeholder.paramTypeDescs = im.functions[i].paramTypeDescs;
    placeholder.returnTypeDesc = im.functions[i].returnTypeDesc;
    for (auto& ptd : placeholder.paramTypeDescs)
        RemapTypeDesc(ptd.type, pm.structMap, pm.classMap);
    RemapTypeDesc(placeholder.returnTypeDesc, pm.structMap,
        pm.classMap);
    m_compiledModule.functions.push_back(std::move(placeholder));
}

//Stage B.2: copy + remap bytecode into each placeholder.
void VmBackend::CopyImportedFunctionBytecode() {
    for (size_t m = 0; m < m_importedModules.size(); ++m) {
        auto& im = m_importedModules[m];
        auto& pm = m_importRemaps[m];
        for (uint32_t i = 0; i < im.functions.size(); ++i) {
            std::vector<uint8_t> bcCopy = im.functions[i].bytecode;
            RemapBytecode(bcCopy, pm);
            //Phase 9d: copy + remap tryBlocks (exceptionClassIdx only —
            //startPc/endPc/handlerPc are byte offsets within the same
            //bytecode buffer, so they don't change across module merge).
            std::vector<TryBlock> tbs = im.functions[i].tryBlocks;
            for (auto& tb : tbs) {
                if (tb.exceptionClassIdx != 0xFFFF) {
                    auto it = pm.classMap.find(tb.exceptionClassIdx);
                    if (it != pm.classMap.end())
                        tb.exceptionClassIdx = static_cast<uint16_t>(it->second);
                }
            }
            uint32_t targetIdx = pm.functionMap[i];
            m_compiledModule.functions[targetIdx].bytecode = std::move(bcCopy);
            m_compiledModule.functions[targetIdx].tryBlocks = std::move(tbs);
        }
    }
}

//Stage B.3: complete class metadata remap (methodIndices + constructorIdx).
void VmBackend::RemapImportedMethodIndices() {
    for (size_t m = 0; m < m_importedModules.size(); ++m) {
        auto& im = m_importedModules[m];
        auto& pm = m_importRemaps[m];
        for (uint32_t i = 0; i < im.classes.size(); ++i) {
            if (pm.classWasPushed.find(i) == pm.classWasPushed.end()) continue;
            uint32_t targetIdx = pm.classMap[i];
            auto& cc = m_compiledModule.classes[targetIdx];
            for (auto& idx : cc.methodIndices)
                if (idx != 0xFFFF) idx = static_cast<uint16_t>(pm.functionMap.at(idx));
            if (cc.constructorIdx != 0xFFFF)
                cc.constructorIdx = static_cast<uint16_t>(pm.functionMap.at(cc.constructorIdx));
        }
    }
}

//Stage B.4: fill m_funcIndexMap[stub] for user-codegen lookup (R3-C).
void VmBackend::BindImportedFunctionStubs() {
    for (auto& kv : m_importedFuncSourceIdx) {
        SnFunction* stub = kv.first;
        uint32_t srcModIdx = kv.second.first;
        uint32_t srcFuncIdx = kv.second.second;
        if (srcModIdx >= m_importRemaps.size()) continue;
        auto& pm = m_importRemaps[srcModIdx];
        auto it = pm.functionMap.find(srcFuncIdx);
        if (it == pm.functionMap.end()) continue;
        m_funcIndexMap[stub] = it->second;
    }
}

} //namespace nlang
