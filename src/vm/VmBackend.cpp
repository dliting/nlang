#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/runtime/Module.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <functional>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

VmBackend::VmBackend() = default;
VmBackend::~VmBackend() = default;

void VmBackend::OnModuleCreate(Module& module) {
    m_compiledModule.name = module.Name().ToString();
}

void VmBackend::GenerateTypes(SnNamespace& root) {
    //VmBackend uses multi-pass compilation in GenerateStatements (register types,
    //then functions, then generate code). Type and data registration happen there,
    //not in these separate phases. LLVM backend uses these phases differently
    //because LLVM IR supports forward references.
}

void VmBackend::GenerateData(SnNamespace& root) {
    //See GenerateTypes comment.
}

void VmBackend::GenerateStatements(SnNamespace& root) {
    RegisterStructs(root);
    RegisterClasses(root);
    ResolveStructClassRefs();
    RegisterArrayTypes(root);
    RegisterFunctions(root);
    PopulateClassMethods(root);
    GenerateAllBytecode(root);
}

void VmBackend::RegisterStructs(SnNamespace& root) {
    auto registerStruct = [&](SnStructDecl& sn) {
        CompiledStruct cs;
        cs.name = sn.Name();
        cs.fieldCount = static_cast<uint16_t>(sn.FieldCount());
        std::vector<std::string> typeNames;
        for (auto& field : sn.Members()) {
            cs.fieldNames.push_back(field.Name());
            auto* fieldType = field.EvalDataType();
            uint16_t ftk = RuntimeTypeKind(fieldType);
            cs.fieldTypeKinds.push_back(ftk);
            cs.fieldStructIndices.push_back(0xFFFF);
            cs.fieldClassIndices.push_back(0xFFFF);
            if ((ftk == RTK_Struct || ftk == RTK_Class) && fieldType)
                typeNames.push_back(fieldType->Name());
            else
                typeNames.push_back("");
        }
        m_compiledModule.structs.push_back(std::move(cs));
        m_structFieldTypeNames.push_back(std::move(typeNames));
    };
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_StructDecl)
            registerStruct(static_cast<SnStructDecl&>(member));
        else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_StructDecl)
                    registerStruct(static_cast<SnStructDecl&>(child));
            }
        }
    }
    //Resolve fieldStructIndices now that all structs are registered.
    //fieldClassIndices are resolved later by ResolveStructClassRefs
    //(after RegisterClasses, since classes are not yet registered here).
    for (size_t si = 0; si < m_compiledModule.structs.size(); ++si) {
        auto& cs = m_compiledModule.structs[si];
        auto& typeNames = m_structFieldTypeNames[si];
        for (size_t i = 0; i < typeNames.size(); ++i) {
            if (!typeNames[i].empty() && cs.fieldTypeKinds[i] == RTK_Struct) {
                int idx = m_compiledModule.FindStruct(typeNames[i]);
                if (idx >= 0)
                    cs.fieldStructIndices[i] = static_cast<uint16_t>(idx);
            }
        }
    }
}

void VmBackend::RegisterClasses(SnNamespace& root) {
    //Map from class name to SnClassDecl* for post-registration resolution.
    std::unordered_map<std::string, SnClassDecl*> declMap;
    auto registerClass = [&](SnClassDecl& sn) {
        CompiledClass cc;
        cc.name = sn.Name();
        cc.superClassIdx = -1;
        //Collect inherited fields: walk from root ancestor to direct parent,
        //collecting each ancestor's own fields (not their inherited fields).
        std::vector<SnClassDecl*> ancestors;
        auto* pSuper = sn.SuperClass();
        while (pSuper) {
            ancestors.push_back(pSuper);
            pSuper = pSuper->SuperClass();
        }
        //Add fields from root ancestor first (reversed order)
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
            for (auto& member : (*it)->Members()) {
                if (member.Kind() == NK_ClassField) {
                    auto& cf = static_cast<SnClassField&>(member);
                    cc.fieldNames.push_back(cf.Name());
                    auto* fieldType = cf.EvalDataType();
                    uint16_t ftk = fieldType ? RuntimeTypeKind(fieldType) : RTK_Int32;
                    cc.fieldTypeKinds.push_back(ftk);
                    cc.fieldStructIndices.push_back(0xFFFF);
                    cc.fieldClassIndices.push_back(0xFFFF);
                    cc.fieldAccess.push_back(static_cast<uint8_t>(cf.Access()));
                }
            }
        }
        //Collect own fields
        for (auto& member : sn.Members()) {
            if (member.Kind() == NK_ClassField) {
                auto& cf = static_cast<SnClassField&>(member);
                cc.fieldNames.push_back(cf.Name());
                auto* fieldType = cf.EvalDataType();
                uint16_t ftk = fieldType ? RuntimeTypeKind(fieldType) : RTK_Int32;
                cc.fieldTypeKinds.push_back(ftk);
                cc.fieldStructIndices.push_back(0xFFFF);
                cc.fieldClassIndices.push_back(0xFFFF);
                cc.fieldAccess.push_back(static_cast<uint8_t>(cf.Access()));
            }
        }
        cc.fieldCount = static_cast<uint16_t>(cc.fieldNames.size());
        declMap[sn.Name()] = &sn;
        m_compiledModule.classes.push_back(std::move(cc));
    };
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_ClassDecl)
            registerClass(static_cast<SnClassDecl&>(member));
        else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_ClassDecl)
                    registerClass(static_cast<SnClassDecl&>(child));
            }
        }
    }
    //Resolve superClassIdx and fieldClassIndices (requires all classes registered).
    for (auto& cc : m_compiledModule.classes) {
        auto it = declMap.find(cc.name);
        if (it == declMap.end()) continue;
        auto* pDecl = it->second;
        if (pDecl->SuperClass()) {
            int idx = m_compiledModule.FindClass(pDecl->SuperClass()->Name());
            cc.superClassIdx = (idx >= 0) ? static_cast<int16_t>(idx) : -1;
        }
        for (size_t i = 0; i < cc.fieldNames.size(); ++i) {
            auto* pField = pDecl->FindField(cc.fieldNames[i]);
            if (pField && pField->Kind() == NK_ClassField) {
                auto* ft = pField->EvalDataType();
                if (ft) {
                    if (cc.fieldTypeKinds[i] == RTK_Class) {
                        int idx = m_compiledModule.FindClass(ft->Name());
                        if (idx >= 0)
                            cc.fieldClassIndices[i] = static_cast<uint16_t>(idx);
                    } else if (cc.fieldTypeKinds[i] == RTK_Struct) {
                        int idx = m_compiledModule.FindStruct(ft->Name());
                        if (idx >= 0)
                            cc.fieldStructIndices[i] = static_cast<uint16_t>(idx);
                    }
                }
            }
        }
    }
}

void VmBackend::ResolveStructClassRefs() {
    for (size_t si = 0; si < m_compiledModule.structs.size(); ++si) {
        auto& cs = m_compiledModule.structs[si];
        auto& typeNames = m_structFieldTypeNames[si];
        for (size_t i = 0; i < typeNames.size(); ++i) {
            if (!typeNames[i].empty() && cs.fieldTypeKinds[i] == RTK_Class) {
                int idx = m_compiledModule.FindClass(typeNames[i]);
                if (idx >= 0)
                    cs.fieldClassIndices[i] = static_cast<uint16_t>(idx);
            }
        }
    }
}

uint16_t VmBackend::RegisterArrayType(SnField* pElemType) {
    uint8_t elemKind = RuntimeTypeKind(pElemType);
    uint16_t elemTypeIdx = 0xFFFF;
    if (elemKind == RTK_Struct && pElemType) {
        int idx = m_compiledModule.FindStruct(pElemType->Name());
        if (idx >= 0)
            elemTypeIdx = static_cast<uint16_t>(idx);
    } else if (elemKind == RTK_Class && pElemType) {
        int idx = m_compiledModule.FindClass(pElemType->Name());
        if (idx >= 0)
            elemTypeIdx = static_cast<uint16_t>(idx);
    }
    int existing = m_compiledModule.FindArray(elemKind, elemTypeIdx);
    if (existing >= 0)
        return static_cast<uint16_t>(existing);
    CompiledArrayType at;
    at.elemKind = elemKind;
    at.elemTypeIdx = elemTypeIdx;
    m_compiledModule.arrayTypes.push_back(at);
    return static_cast<uint16_t>(m_compiledModule.arrayTypes.size() - 1);
}

//Recursively walk the AST collecting array type usages.
//Array types appear in: LocalDeclStmt.Type() when IsArrayType(), NewArrayExpr,
//struct/class fields, function params, return types. For Phase 4 simplicity,
//we scan LocalDeclStmt, struct fields, class fields, formal params, and
//NewArrayExpr. The latter is registered lazily at codegen time.
void VmBackend::RegisterArrayTypes(SnNamespace& root) {
    auto processType = [&](SnFieldExpr* pTypeExpr) {
        if (pTypeExpr && pTypeExpr->IsArrayType()) {
            //Walk through nested SnArrayTypeExpr nodes to find the element type.
            auto* pCur = pTypeExpr;
            while (pCur->Kind() == NK_ArrayTypeExpr) {
                pCur = static_cast<SnArrayTypeExpr*>(pCur)->ElementType();
            }
            //pCur is now the base type (SnNameExpr for primitives/classes).
            if (auto* pNameExpr = dynamic_cast<SnNameExpr*>(pCur)) {
                if (pNameExpr->Field())
                    RegisterArrayType(pNameExpr->Field());
            }
        }
    };
    std::function<void(SnField&)> walkField = [&](SnField& f) {
        if (f.Kind() == NK_StructField) {
            auto& sf = static_cast<SnStructField&>(f);
            if (sf.Type())
                processType(sf.Type());
        }
        if (f.Kind() == NK_ClassField) {
            auto& cf = static_cast<SnClassField&>(f);
            if (cf.Type())
                processType(cf.Type());
        }
        if (f.Kind() == NK_FormalParam) {
            auto& fp = static_cast<SnFormalParam&>(f);
            if (fp.Type())
                processType(fp.Type());
        }
    };
    std::function<void(SnStatement&)> walkStmt = [&](SnStatement& s) {
        if (s.Kind() == NK_Paragraph) {
            for (auto& child : static_cast<SnParagraph&>(s).Statements())
                walkStmt(child);
        } else if (s.Kind() == NK_LocalDeclStmt) {
            auto& decl = static_cast<SnLocalDeclStmt&>(s);
            if (decl.Type() && decl.Type()->IsArrayType())
                processType(decl.Type());
        } else if (s.Kind() == NK_IfStmt) {
            auto& ifStmt = static_cast<SnIfStmt&>(s);
            if (ifStmt.ThenStmt()) walkStmt(*ifStmt.ThenStmt());
            if (ifStmt.ElseStmt()) walkStmt(*ifStmt.ElseStmt());
        } else if (s.Kind() == NK_WhileStmt) {
            walkStmt(*static_cast<SnWhileStmt&>(s).Body());
        } else if (s.Kind() == NK_DoStmt) {
            walkStmt(*static_cast<SnDoStmt&>(s).Body());
        } else if (s.Kind() == NK_ForStmt) {
            auto& forStmt = static_cast<SnForStmt&>(s);
            if (forStmt.Init()) walkStmt(*forStmt.Init());
            if (forStmt.Body()) walkStmt(*forStmt.Body());
        }
    };
    std::function<void(SyntaxNode&)> walkNode = [&](SyntaxNode& n) {
        if (n.Kind() == NK_StructDecl) {
            for (auto& member : static_cast<SnStructDecl&>(n).Members())
                walkField(member);
        } else if (n.Kind() == NK_ClassDecl) {
            for (auto& member : static_cast<SnClassDecl&>(n).Members())
                walkField(member);
        } else if (n.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(n);
            for (auto& param : func.Params())
                walkField(param);
            if (func.Body())
                walkStmt(*func.Body());
        }
    };
    for (auto& member : root.Members()) {
        walkNode(member);
        if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members())
                walkNode(child);
        }
    }
}

void VmBackend::RegisterFunctions(SnNamespace& root) {
    m_funcIndexMap.clear();
    size_t funcRegIdx = 0;
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(member);
            if (!func.Body())
                continue;
            CompiledFunction cf;
            cf.name = func.Name();
            m_compiledModule.functions.push_back(std::move(cf));
            m_funcIndexMap[&func] = funcRegIdx++;
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_Function) {
                    auto& func = static_cast<SnFunction&>(child);
                    if (!func.Body())
                        continue;
                    CompiledFunction cf;
                    cf.name = func.Name();
                    m_compiledModule.functions.push_back(std::move(cf));
                    m_funcIndexMap[&func] = funcRegIdx++;
                }
            }
        }
    }
}

void VmBackend::PopulateClassMethods(SnNamespace& root) {
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_ClassDecl) {
            auto& sn = static_cast<SnClassDecl&>(member);
            int ccIdx = m_compiledModule.FindClass(sn.Name());
            if (ccIdx < 0) continue;
            auto& cc = m_compiledModule.classes[static_cast<size_t>(ccIdx)];
            cc.methodIndices.clear();
            cc.constructorIdx = 0xFFFF;
            for (auto& child : sn.Members()) {
                if (child.Kind() == NK_Function) {
                    auto it = m_funcIndexMap.find(static_cast<SnFunction*>(&child));
                    if (it != m_funcIndexMap.end()) {
                        uint16_t funcIdx = static_cast<uint16_t>(it->second);
                        cc.methodIndices.push_back(funcIdx);
                        if (child.Name() == sn.Name())
                            cc.constructorIdx = funcIdx;
                    }
                }
            }
        }
    }
}

void VmBackend::GenerateAllBytecode(SnNamespace& root) {
    size_t funcIdx = 0;
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(member);
            if (!func.Body())
                continue;
            GenerateFunction(func, funcIdx);
            ++funcIdx;
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_Function) {
                    auto& func = static_cast<SnFunction&>(child);
                    if (!func.Body())
                        continue;
                    GenerateFunction(func, funcIdx);
                    ++funcIdx;
                }
            }
        }
    }
}

//Map compile-time type kind to runtime type kind.
//Enum types are int32 at runtime. Struct types use RTK_Struct.
//Array types are detected via SnField::IsArrayType() (overridden by
//SnArrayTypeExpr to return true), not via the resolved element type.
uint8_t VmBackend::RuntimeTypeKind(SnField* pType) {
    if (!pType) return RTK_Int32;
    if (pType->IsArrayType()) return RTK_Array;
    auto k = pType->Kind();
    if (k == NK_EnumDecl) return RTK_Int32;
    if (k == NK_StructDecl) return RTK_Struct;
    if (k == NK_ClassDecl) return RTK_Class;
    return static_cast<uint8_t>(k);
}

uint16_t VmBackend::PickTempSlot(uint16_t exclude) const {
    return (exclude == m_currFunc->tempSlot)
        ? m_currFunc->tempSlot2 : m_currFunc->tempSlot;
}

//Returns field offset in bytes, or -1 if not found.
static int FindFieldOffset(SnStructDecl& structDecl, const std::string& fieldName) {
    uint16_t off = 0;
    for (auto& sf : structDecl.Members()) {
        if (sf.Name() == fieldName)
            return off;
        off += VALUE_SIZE;
    }
    return -1;
}

//Returns class field offset in bytes (including +4 for classIdx slot), or -1.
//Object layout: [classIdx, root_ancestor_fields..., parent_fields..., own_fields...]
//Slot[0] = classIdx (4 bytes, used for runtime virtual dispatch via OP_CallMethod).
//Slot[1..N] = data fields (4 bytes each, offset starts at VALUE_SIZE=4).
//Compile-time offset here must match runtime AllocClassOnHeap in VmExecutor.
static int FindClassFieldOffset(SnClassDecl& classDecl, const std::string& fieldName) {
    //Collect ancestor chain from root to direct parent
    std::vector<SnClassDecl*> ancestors;
    auto* pSuper = classDecl.SuperClass();
    while (pSuper) {
        ancestors.push_back(pSuper);
        pSuper = pSuper->SuperClass();
    }
    //Search from root ancestor to direct parent (reversed order)
    uint16_t off = VALUE_SIZE; //skip classIdx slot
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        for (auto& member : (*it)->Members()) {
            if (member.Kind() == NK_ClassField && member.Name() == fieldName)
                return off;
            if (member.Kind() == NK_ClassField)
                off += VALUE_SIZE;
        }
    }
    //Search in this class (after all parent fields)
    for (auto& member : classDecl.Members()) {
        if (member.Kind() == NK_ClassField && member.Name() == fieldName)
            return off;
        if (member.Kind() == NK_ClassField)
            off += VALUE_SIZE;
    }
    return -1;
}

uint16_t VmBackend::AddStringConstant(const std::string& s) {
    auto& pool = m_compiledModule.stringConstants;
    for (uint16_t i = 0; i < static_cast<uint16_t>(pool.size()); ++i) {
        if (pool[i] == s)
            return i;
    }
    assert(pool.size() < UINT16_MAX && "string constant pool overflow");
    pool.push_back(s);
    return static_cast<uint16_t>(pool.size() - 1);
}

void VmBackend::GenerateFunction(SnFunction& func, size_t funcIdx) {
    CompiledFunction& compiledFunc = m_compiledModule.functions[funcIdx];

    FuncContext ctx;
    ctx.func = &compiledFunc;
    ctx.nextOffset = 0;
    m_currFunc = &ctx;

    // If this is a class method, allocate slot 0 for the 'this' pointer.
    bool isMethod = func.Parent() && func.Parent()->Kind() == NK_ClassDecl;
    if (isMethod) {
        AllocLocal("__this", VALUE_SIZE, RTK_Class, true);
    }

    // Allocate slots for parameters
    for (auto& param : func.Params()) {
        AllocLocal(param.Name(), VALUE_SIZE,
                   RuntimeTypeKind(param.EvalDataType()),
                   true);
    }
    compiledFunc.paramCount = static_cast<uint16_t>(
        func.Params().size() + (isMethod ? 1 : 0));

    // Return type
    if (func.HasReturn() && func.ReturnType()) {
        auto* retType = func.ReturnType()->Field();
        compiledFunc.returnTypeKind = retType
            ? static_cast<uint16_t>(RuntimeTypeKind(retType)) : 0;
        ctx.returnSlot = ctx.nextOffset;
        ctx.nextOffset += VALUE_SIZE;
    }

    // Temporary slots for binary operations
    ctx.tempSlot = ctx.nextOffset;
    ctx.nextOffset += VALUE_SIZE;
    ctx.tempSlot2 = ctx.nextOffset;
    ctx.nextOffset += VALUE_SIZE;

    // Call parameter area (8 slots = up to 8 parameters)
    ctx.callParamBase = ctx.nextOffset;
    ctx.nextOffset += 8 * VALUE_SIZE;

    // Generate bytecode for body
    BytecodeEmitter emitter;
    if (func.Body()) {
        for (auto& stmt : func.Body()->Statements()) {
            EmitStatement(stmt, emitter);
        }
    }

    // Append implicit return (fallback for functions that don't hit an
    // explicit return statement — e.g. void functions, or fall-through).
    if (func.HasReturn()) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(ctx.returnSlot);
    }
    emitter.Emit(OpCode::OP_Return);

    compiledFunc.bytecode = emitter.TakeBytes();
    compiledFunc.localsSize = ctx.nextOffset;

    m_currFunc = nullptr;
}

void VmBackend::EmitExpression(SnExpression& expr, BytecodeEmitter& emitter,
                                uint16_t resultOffset) {
    NodeKind kind = expr.Kind();

    if (kind == NK_LiteralExpr) {
        auto& lit = static_cast<SnLiteralExpr&>(expr);
        auto* evalType = lit.EvalDataType();

        if (!evalType) {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }

        NodeKind typeKind = evalType->Kind();
        if (typeKind == NK_Int32) {
            int32_t v = lit.Value().Get<int32_t>();
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(v);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        } else if (typeKind == NK_Float) {
            float v = lit.Value().Get<float>();
            emitter.Emit(OpCode::OP_ConstFloat);
            emitter.EmitFloat(v);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        } else if (typeKind == NK_String) {
            auto* pStr = lit.Value().Data().m_String;
            std::string sVal = pStr ? *pStr : "";
            uint16_t poolIdx = AddStringConstant(sVal);
            emitter.Emit(OpCode::OP_ConstString);
            emitter.EmitUint16(poolIdx);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        } else {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        }
        return;
    }

    if (kind == NK_IdentifierExpr) {
        auto& idExpr = static_cast<SnIdentifierExpr&>(expr);
        auto* field = idExpr.Field();
        if (field && field->Kind() == NK_EnumMember) {
            //Enum member constant — emit the resolved integer value.
            auto* pEnumMember = static_cast<SnEnumMember*>(field);
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(pEnumMember->Value());
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        if (field) {
            uint16_t offset = FindLocal(field->Name());
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(offset);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        } else {
            //Unresolved identifier — write zero as fallback.
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        }
        return;
    }

    if (kind == NK_InvokeExpr) {
        auto& invoke = static_cast<SnInvokeExpr&>(expr);
        auto* callee = invoke.Callee();

        // Evaluate parameters into call parameter area
        {
            uint16_t paramIdx = 0;
            for (auto& param : invoke.Params()) {
                uint16_t paramOffset = m_currFunc->callParamBase + paramIdx * VALUE_SIZE;
                EmitExpression(param, emitter, paramOffset);
                //If the parameter is a struct type, deep-copy it so the
                //callee gets its own heap slot tree.
                auto* paramType = param.EvalDataType();
                if (paramType && RuntimeTypeKind(paramType) == RTK_Struct) {
                    int structIdx = m_compiledModule.FindStruct(paramType->Name());
                    //Copy from paramOffset to a temp, then back.
                    //We need a temp slot that won't conflict.
                    //Use tempSlot as intermediate.
                    emitter.Emit(OpCode::OP_CopyStruct);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(paramOffset);
                    emitter.EmitUint16(structIdx >= 0
                        ? static_cast<uint16_t>(structIdx) : 0);
                    //Move the new heap index back to paramOffset
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(paramOffset);
                }
                ++paramIdx;
            }
        }

        // Find function index
        int funcIndex = -1;
        if (callee) {
            auto it = m_funcIndexMap.find(callee);
            if (it != m_funcIndexMap.end())
                funcIndex = static_cast<int>(it->second);
        }
        //callee == null means unresolved invoke — skip (compiler should have reported error)
        if (funcIndex >= 0) {
            emitter.Emit(OpCode::OP_CallFunc);
            emitter.EmitUint16(static_cast<uint16_t>(funcIndex));
            emitter.EmitUint16(m_currFunc->callParamBase);
            // Result is in pResult, store to resultOffset
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
        }
        emitter.Emit(OpCode::OP_ParaEnd);
        return;
    }

    if (kind == NK_CastExpr) {
        auto& cast = static_cast<SnCastExpr&>(expr);
        EmitExpression(*cast.Source(), emitter, resultOffset);

        auto* targetType = cast.Target();
        auto* sourceType = cast.Source()->EvalDataType();
        if (sourceType && targetType) {
            NodeKind srcKind = sourceType->Kind();
            NodeKind dstKind = targetType->Kind();
            if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
            if (dstKind == NK_EnumDecl) dstKind = NK_Int32;
            if (srcKind == NK_Int32 && dstKind == NK_Float) {
                emitter.Emit(OpCode::OP_CastIntToFloat);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            } else if (srcKind == NK_Float && dstKind == NK_Int32) {
                emitter.Emit(OpCode::OP_CastFloatToInt);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            }
        }
        return;
    }

    // Member expression - struct field access or delegate to inner
    if (kind == NK_MemberExpr) {
        auto& member = static_cast<SnMemberExpr&>(expr);
        auto* field = member.Field();
        if (field && field->Kind() == NK_EnumMember) {
            //Enum member constant (e.g. Color.Red).
            auto* pEnumMember = static_cast<SnEnumMember*>(field);
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(pEnumMember->Value());
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        //Struct field access (e.g. pt.x, pt.inner.x)
        auto* outerType = member.Outer()->EvalDataType();
        if (outerType && outerType->Kind() == NK_StructDecl) {
            auto* structDecl = static_cast<SnStructDecl*>(outerType);
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Find the field's offset within the struct
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int off = FindFieldOffset(*structDecl, fieldName);
                if (off < 0) return; //should not happen after type resolution
                emitter.Emit(OpCode::OP_LoadField);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(off));
            }
            return;
        }
        //Class field access (e.g. obj.x, obj.y)
        if (outerType && outerType->Kind() == NK_ClassDecl) {
            auto* classDecl = static_cast<SnClassDecl*>(outerType);
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Null check
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(resultOffset);
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int off = FindClassFieldOffset(*classDecl, fieldName);
                if (off < 0) return;
                emitter.Emit(OpCode::OP_LoadField);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(off));
            } else if (inner && inner->Kind() == NK_InvokeExpr) {
                //Class method call
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);
                //Evaluate args to call param area (slot 0 = this)
                uint16_t paramIdx = 1;
                for (auto& param : invoke.Params()) {
                    uint16_t paramOffset = m_currFunc->callParamBase + paramIdx * VALUE_SIZE;
                    EmitExpression(param, emitter, paramOffset);
                    ++paramIdx;
                }
                //Copy this to callParamBase[0]
                emitter.Emit(OpCode::OP_VarLocal);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(m_currFunc->callParamBase);
                //Find the method function
                auto* callee = invoke.Callee();
                bool isVirtual = callee && callee->ContainFlags(NF_Virtual);
                if (isVirtual) {
                    //Virtual method dispatch — name-based lookup at runtime
                    //(like EN's I_Base_CallVirtualFunc + FindFunctionChecked)
                    uint16_t nameIdx = AddStringConstant(callee->Name());
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(nameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                } else if (callee) {
                    //Non-virtual (final) method — direct call by function index
                    //(like EN's I_Base_CallFinalFunc + NFunction*)
                    auto it = m_funcIndexMap.find(callee);
                    if (it != m_funcIndexMap.end()) {
                        emitter.Emit(OpCode::OP_CallMethodDirect);
                        emitter.EmitUint16(static_cast<uint16_t>(it->second));
                        emitter.EmitUint16(m_currFunc->callParamBase);
                    }
                }
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_ParaEnd);
            }
            return;
        }
        //Interface method call (e.g. p.Print()).
        //At runtime, p holds a heap reference whose actual class is unknown
        //at compile time. Always dispatch virtually by method name — the
        //VM walks the runtime class's methodIndices via superClassIdx.
        if (outerType && outerType->Kind() == NK_InterfaceDecl) {
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Null check
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(resultOffset);
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_InvokeExpr) {
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);
                //Evaluate args to call param area (slot 0 = this)
                uint16_t paramIdx = 1;
                for (auto& param : invoke.Params()) {
                    uint16_t paramOffset = m_currFunc->callParamBase + paramIdx * VALUE_SIZE;
                    EmitExpression(param, emitter, paramOffset);
                    ++paramIdx;
                }
                //Copy this to callParamBase[0]
                emitter.Emit(OpCode::OP_VarLocal);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(m_currFunc->callParamBase);
                //Always virtual dispatch by name.
                auto* callee = invoke.Callee();
                if (callee) {
                    uint16_t nameIdx = AddStringConstant(callee->Name());
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(nameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                }
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_ParaEnd);
            }
            return;
        }
        auto* inner = member.Inner();
        //Array.length builtin property (e.g. arr.length)
        //The outer expression refers to an array-typed field/local; check
        //its IsArrayType() flag (forwarded from the type's SnNameExpr).
        if (inner && inner->Kind() == NK_IdentifierExpr) {
            auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
            SnField* outerField = nullptr;
            if (member.Outer()->Kind() == NK_IdentifierExpr)
                outerField = static_cast<SnIdentifierExpr*>(
                    member.Outer())->Field();
            if (outerField && outerField->IsArrayType()
                && fieldName == "length")
            {
                EmitExpression(*member.Outer(), emitter, resultOffset);
                emitter.Emit(OpCode::OP_NullCheck);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_ArrayLength);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                return;
            }
        }
        //Array element field access: arr[i].field (struct element)
        if (member.Outer()->Kind() == NK_SubscriptExpr) {
            auto& sub = static_cast<SnSubscriptExpr&>(*member.Outer());
            auto* subElemType = sub.EvalDataType();
            if (subElemType && subElemType->Kind() == NK_StructDecl) {
                auto* structDecl = static_cast<SnStructDecl*>(subElemType);
                //Evaluate array ref + index, load element heap index
                EmitExpression(sub, emitter, resultOffset);
                if (inner && inner->Kind() == NK_IdentifierExpr) {
                    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                    int off = FindFieldOffset(*structDecl, fieldName);
                    if (off < 0) return;
                    emitter.Emit(OpCode::OP_LoadField);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                }
                return;
            }
            //Class element field access: arr[i].field (class element)
            if (subElemType && subElemType->Kind() == NK_ClassDecl) {
                auto* classDecl = static_cast<SnClassDecl*>(subElemType);
                EmitExpression(sub, emitter, resultOffset);
                emitter.Emit(OpCode::OP_NullCheck);
                emitter.EmitUint16(resultOffset);
                if (inner && inner->Kind() == NK_IdentifierExpr) {
                    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                    int off = FindClassFieldOffset(*classDecl, fieldName);
                    if (off < 0) return;
                    emitter.Emit(OpCode::OP_LoadField);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                }
                return;
            }
        }
        //String builtin methods: s.length()
        if (inner && inner->Kind() == NK_InvokeExpr) {
            auto& invoke = static_cast<SnInvokeExpr&>(*inner);
            if (outerType && outerType->Kind() == NK_String
                && invoke.CalleeName() == "length")
            {
                EmitExpression(*member.Outer(), emitter, resultOffset);
                emitter.Emit(OpCode::OP_StrLen);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                return;
            }
            EmitExpression(*static_cast<SnExpression*>(inner), emitter, resultOffset);
        }
        return;
    }

    // Name expression - delegate
    if (kind == NK_NameExpr) {
        auto& nameExpr = static_cast<SnNameExpr&>(expr);
        if (nameExpr.Expr()) {
            EmitExpression(*nameExpr.Expr(), emitter, resultOffset);
        }
        return;
    }

    // New expression - object instantiation
    if (kind == NK_NewExpr) {
        auto& newExpr = static_cast<SnNewExpr&>(expr);
        auto* pClassDecl = newExpr.ClassDecl();
        if (!pClassDecl) {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        int classIdx = m_compiledModule.FindClass(pClassDecl->Name());
        if (classIdx < 0) {
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        emitter.Emit(OpCode::OP_New);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(static_cast<uint16_t>(classIdx));
        //Call constructor if present (direct class only, using pre-computed index).
        //Ancestor constructors are NOT called because NLang has no super()
        //syntax to pass arguments to them.
        uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
        if (ctorIdx != 0xFFFF) {
            uint16_t paramIdx = 1; //slot 0 = this
            for (auto& param : newExpr.Args()) {
                if (param.Kind() == NK_NameExpr) continue;
                uint16_t paramOffset = m_currFunc->callParamBase + paramIdx * VALUE_SIZE;
                EmitExpression(param, emitter, paramOffset);
                ++paramIdx;
            }
            //Copy this (new object heap index) to callParamBase[0]
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(resultOffset);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->callParamBase);
            emitter.Emit(OpCode::OP_CallMethodDirect);
            emitter.EmitUint16(ctorIdx);
            emitter.EmitUint16(m_currFunc->callParamBase);
            emitter.Emit(OpCode::OP_ParaEnd);
        }
        return;
    }

    // This expression - reads the implicit first parameter
    if (kind == NK_ThisExpr) {
        //this is the first parameter (offset 0)
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(0);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return;
    }

    // New array expression: new T[size]
    if (kind == NK_NewArrayExpr) {
        auto& newArr = static_cast<SnNewArrayExpr&>(expr);
        //Register the array type (idempotent)
        uint16_t arrayTypeIdx = RegisterArrayType(
            newArr.ElementType()->Field());
        //Evaluate size expression to tempSlot (size is int32)
        EmitExpression(*newArr.Size(), emitter, m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_AllocArray);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(arrayTypeIdx);
        emitter.EmitUint16(m_currFunc->tempSlot);
        return;
    }

    // Subscript expression: arr[index]
    if (kind == NK_SubscriptExpr) {
        auto& sub = static_cast<SnSubscriptExpr&>(expr);
        //Evaluate array reference to resultOffset
        EmitExpression(*sub.Array(), emitter, resultOffset);
        //Null check
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(resultOffset);
        //Evaluate index to a slot that does NOT alias resultOffset —
        //otherwise the index eval would clobber the array heap index when
        //resultOffset happens to be tempSlot (e.g. when this subscript is
        //the right operand of a binary expression).
        uint16_t indexSlot = PickTempSlot(resultOffset);
        EmitExpression(*sub.Index(), emitter, indexSlot);
        emitter.Emit(OpCode::OP_LoadElement);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(indexSlot);
        //For struct element types, deep-copy on read (value semantics)
        auto* elemType = sub.EvalDataType();
        if (elemType && RuntimeTypeKind(elemType) == RTK_Struct) {
            int structIdx = m_compiledModule.FindStruct(elemType->Name());
            emitter.Emit(OpCode::OP_CopyStruct);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(structIdx >= 0
                ? static_cast<uint16_t>(structIdx) : 0);
        }
        return;
    }

    // Binary/unary operator expression
    if (kind == NK_BinaryExpr) {
        auto& bin = static_cast<SnBinaryExpr&>(expr);
        auto op = bin.Op();

        if (op == SnBinaryExpr::OP_Neg || op == SnBinaryExpr::OP_LogicalNot) {
            //Unary: evaluate operand to resultOffset, then negate in-place
            EmitExpression(*bin.Left(), emitter, resultOffset);
            if (op == SnBinaryExpr::OP_Neg) {
                auto* evalType = bin.Left()->EvalDataType();
                if (evalType && evalType->Kind() == NK_Float)
                    emitter.Emit(OpCode::OP_Neg_f32);
                else
                    emitter.Emit(OpCode::OP_Neg_i32);
                emitter.EmitUint16(resultOffset);
            } else {
                emitter.Emit(OpCode::OP_LogicalNot);
                emitter.EmitUint16(resultOffset);
            }
            return;
        }

        //Binary: evaluate left to resultOffset, right to a different slot, then apply op.
        //rightSlot must differ from resultOffset to avoid the right operand
        //overwriting the left before the binary op executes. PickTempSlot
        //centralizes the "two temp slots, alternate on conflict" convention.
        //This composes for nested expressions: each level derives its own
        //rightSlot from its own resultOffset, so subexprs naturally alternate
        //between tempSlot and tempSlot2.
        uint16_t rightSlot = PickTempSlot(resultOffset);
        EmitExpression(*bin.Left(), emitter, resultOffset);
        EmitExpression(*bin.Right(), emitter, rightSlot);

        auto* evalType = bin.Left()->EvalDataType();
        bool isFloat = evalType && evalType->Kind() == NK_Float;
        bool isString = evalType && evalType->Kind() == NK_String;

        switch (op) {
        case SnBinaryExpr::OP_Add:
            if (isString)
                emitter.Emit(OpCode::OP_Concat_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_Add_f32 : OpCode::OP_Add_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Sub:
            emitter.Emit(isFloat ? OpCode::OP_Sub_f32 : OpCode::OP_Sub_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Mul:
            emitter.Emit(isFloat ? OpCode::OP_Mul_f32 : OpCode::OP_Mul_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Div:
            emitter.Emit(isFloat ? OpCode::OP_Div_f32 : OpCode::OP_Div_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Mod:
            emitter.Emit(OpCode::OP_Mod_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Less:
            emitter.Emit(isFloat ? OpCode::OP_Less_f32 : OpCode::OP_Less_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_LessEqual:
            emitter.Emit(isFloat ? OpCode::OP_LessEqual_f32 : OpCode::OP_LessEqual_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Greater:
            emitter.Emit(isFloat ? OpCode::OP_Greater_f32 : OpCode::OP_Greater_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_GreaterEqual:
            emitter.Emit(isFloat ? OpCode::OP_GreaterEqual_f32 : OpCode::OP_GreaterEqual_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Equal:
            if (isString)
                emitter.Emit(OpCode::OP_Eq_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_Equal_f32 : OpCode::OP_Equal_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_NotEqual:
            if (isString)
                emitter.Emit(OpCode::OP_Ne_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_NotEqual_f32 : OpCode::OP_NotEqual_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_LogicalAnd:
            emitter.Emit(OpCode::OP_LogicalAnd);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_LogicalOr:
            emitter.Emit(OpCode::OP_LogicalOr);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        default:
            throw std::runtime_error(
                "NLang backend: unsupported binary operator");
        }
        return;
    }

    // Fallback: write zero
    emitter.Emit(OpCode::OP_ConstZero);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

void VmBackend::EmitStatement(SnStatement& stmt, BytecodeEmitter& emitter) {
    NodeKind kind = stmt.Kind();

    if (kind == NK_ReturnStmt) {
        auto& ret = static_cast<SnReturnStmt&>(stmt);
        if (ret.Result()) {
            EmitExpression(*ret.Result(), emitter, m_currFunc->returnSlot);
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(m_currFunc->returnSlot);
        }
        emitter.Emit(OpCode::OP_Return);
        return;
    }

    if (kind == NK_InvokeStmt) {
        auto& invoke = static_cast<SnInvokeStmt&>(stmt);
        EmitExpression(*invoke.Expr(), emitter, m_currFunc->tempSlot);
        return;
    }

    if (kind == NK_Paragraph) {
        auto& para = static_cast<SnParagraph&>(stmt);
        for (auto& child : para.Statements())
            EmitStatement(child, emitter);
        return;
    }

    //Local variable declaration.
    //After the decomposition pattern, initializers are handled by
    //AssignStmts inserted after this declaration. We only allocate
    //the local variable slot here.
    //Reference: EN's LocalDeclStmt::Compile (SeStatements.cpp:226).
    if (kind == NK_LocalDeclStmt) {
        auto& decl = static_cast<SnLocalDeclStmt&>(stmt);
        //Detect array type via IsArrayType() on the type expression.
        bool isArrayType = decl.Type()->IsArrayType();
        uint8_t typeKind;
        SnField* evalType = nullptr;
        if (isArrayType) {
            typeKind = RTK_Array;
            //Walk through SnArrayTypeExpr to find the element type.
            auto* pCur = decl.Type();
            while (pCur->Kind() == NK_ArrayTypeExpr)
                pCur = static_cast<SnArrayTypeExpr*>(pCur)->ElementType();
            if (auto* pNameExpr = dynamic_cast<SnNameExpr*>(pCur))
                evalType = pNameExpr->Field();
        } else {
            evalType = decl.Type()->Field();
            typeKind = RuntimeTypeKind(evalType);
        }
        for (auto& local : decl.Decls()) {
            uint16_t offset = AllocLocal(local.name, VALUE_SIZE, typeKind, false);
            //For struct types, emit OP_AllocStruct to allocate on heap.
            if (typeKind == RTK_Struct && evalType) {
                int structIdx = m_compiledModule.FindStruct(evalType->Name());
                if (structIdx >= 0) {
                    auto& cs = m_compiledModule.structs[structIdx];
                    emitter.Emit(OpCode::OP_AllocStruct);
                    emitter.EmitUint16(offset);
                    emitter.EmitUint16(static_cast<uint16_t>(structIdx));
                    emitter.EmitUint16(cs.fieldCount);
                }
            }
            //Class and array types start as null (0) — no allocation needed.
        }
        return;
    }

    //Assignment statement.
    //Reference: EN's AssignStmt::Compile (SeStatements.cpp:313).
    if (kind == NK_AssignStmt) {
        auto& assign = static_cast<SnAssignStmt&>(stmt);
        if (assign.Left()->Kind() == NK_IdentifierExpr) {
            auto& idExpr = static_cast<SnIdentifierExpr&>(*assign.Left());
            auto* field = idExpr.Field();
            if (field) {
                uint16_t offset = FindLocal(field->Name());
                auto* varType = field->EvalDataType();
                if (varType && RuntimeTypeKind(varType) == RTK_Struct) {
                    //Struct assignment: evaluate right to temp, then deep-copy.
                    EmitExpression(*assign.Right(), emitter, m_currFunc->tempSlot2);
                    int structIdx = m_compiledModule.FindStruct(varType->Name());
                    emitter.Emit(OpCode::OP_CopyStruct);
                    emitter.EmitUint16(offset);
                    emitter.EmitUint16(m_currFunc->tempSlot2);
                    emitter.EmitUint16(structIdx >= 0
                        ? static_cast<uint16_t>(structIdx) : 0);
                } else {
                    EmitExpression(*assign.Right(), emitter, offset);
                }
            }
        } else if (assign.Left()->Kind() == NK_MemberExpr) {
            //Struct or class field assignment
            auto& memberExpr = static_cast<SnMemberExpr&>(*assign.Left());
            //Specialized handling for arr[i].field = value:
            //the subscript outer's index must NOT alias tempSlot2 (which holds
            //the right side value). Emit the subscript inline using
            //callParamBase for the index slot.
            if (memberExpr.Outer()->Kind() == NK_SubscriptExpr) {
                auto& sub = static_cast<SnSubscriptExpr&>(
                    *memberExpr.Outer());
                auto* elemType = sub.EvalDataType();
                if (elemType
                    && (elemType->Kind() == NK_ClassDecl
                        || elemType->Kind() == NK_StructDecl))
                {
                    auto* inner = memberExpr.Inner();
                    if (inner->Kind() != NK_IdentifierExpr) return;
                    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                    uint16_t fieldOff = 0;
                    if (elemType->Kind() == NK_ClassDecl) {
                        int off = FindClassFieldOffset(
                            *static_cast<SnClassDecl*>(elemType), fieldName);
                        if (off < 0) return;
                        fieldOff = static_cast<uint16_t>(off);
                    } else {
                        int off = FindFieldOffset(
                            *static_cast<SnStructDecl*>(elemType), fieldName);
                        if (off < 0) return;
                        fieldOff = static_cast<uint16_t>(off);
                    }
                    //1. Evaluate right side → tempSlot2 (value preserved)
                    EmitExpression(*assign.Right(), emitter,
                        m_currFunc->tempSlot2);
                    //2. Evaluate array ref → tempSlot
                    EmitExpression(*sub.Array(), emitter,
                        m_currFunc->tempSlot);
                    emitter.Emit(OpCode::OP_NullCheck);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    //3. Evaluate index → callParamBase (NOT tempSlot2)
                    EmitExpression(*sub.Index(), emitter,
                        m_currFunc->callParamBase);
                    //4. load_element dst=tempSlot arr=tempSlot idx=callParamBase
                    emitter.Emit(OpCode::OP_LoadElement);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    //5. null_check the element (class) or skip (struct is value)
                    if (elemType->Kind() == NK_ClassDecl) {
                        emitter.Emit(OpCode::OP_NullCheck);
                        emitter.EmitUint16(m_currFunc->tempSlot);
                    }
                    //6. store_field obj=tempSlot off=fieldOff src=tempSlot2
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(fieldOff);
                    emitter.EmitUint16(m_currFunc->tempSlot2);
                    return;
                }
            }
            auto* outerType = memberExpr.Outer()->EvalDataType();
            if (outerType && outerType->Kind() == NK_ClassDecl) {
                //Class field assignment: reference semantics, no deep copy
                auto* classDecl = static_cast<SnClassDecl*>(outerType);
                auto* inner = memberExpr.Inner();
                if (inner->Kind() != NK_IdentifierExpr) return;
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int fieldOffInt = FindClassFieldOffset(*classDecl, fieldName);
                if (fieldOffInt < 0) return;
                uint16_t fieldOff = static_cast<uint16_t>(fieldOffInt);
                //Evaluate right side to tempSlot2
                EmitExpression(*assign.Right(), emitter, m_currFunc->tempSlot2);
                //Evaluate outer to tempSlot (class object heap index)
                EmitExpression(*memberExpr.Outer(), emitter, m_currFunc->tempSlot);
                emitter.Emit(OpCode::OP_NullCheck);
                emitter.EmitUint16(m_currFunc->tempSlot);
                emitter.Emit(OpCode::OP_StoreField);
                emitter.EmitUint16(m_currFunc->tempSlot);
                emitter.EmitUint16(fieldOff);
                emitter.EmitUint16(m_currFunc->tempSlot2);
            } else if (outerType && outerType->Kind() == NK_StructDecl) {
                auto* structDecl = static_cast<SnStructDecl*>(outerType);
                //Find the field's offset and type
                auto* inner = memberExpr.Inner();
                if (inner->Kind() != NK_IdentifierExpr) return;
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int fieldOffInt = FindFieldOffset(*structDecl, fieldName);
                if (fieldOffInt < 0) return;
                uint16_t fieldOff = static_cast<uint16_t>(fieldOffInt);
                SnField* fieldType = nullptr;
                for (auto& sf : structDecl->Members()) {
                    if (sf.Name() == fieldName) {
                        fieldType = sf.EvalDataType();
                        break;
                    }
                }
                //Evaluate right side to tempSlot2
                EmitExpression(*assign.Right(), emitter, m_currFunc->tempSlot2);
                if (fieldType && RuntimeTypeKind(fieldType) == RTK_Struct) {
                    //Struct-to-struct field assignment: deep-copy first
                    int fieldStructIdx = m_compiledModule.FindStruct(
                        fieldType->Name());
                    emitter.Emit(OpCode::OP_CopyStruct);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(m_currFunc->tempSlot2);
                    emitter.EmitUint16(fieldStructIdx >= 0
                        ? static_cast<uint16_t>(fieldStructIdx) : 0);
                    //Evaluate outer to tempSlot2 (parent struct's heap index)
                    EmitExpression(*memberExpr.Outer(), emitter, m_currFunc->tempSlot2);
                    //Store the new heap index into the parent's field
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(m_currFunc->tempSlot2);
                    emitter.EmitUint16(fieldOff);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                } else {
                    //Primitive/enum/string field assignment
                    //Evaluate outer to tempSlot (parent struct's heap index)
                    EmitExpression(*memberExpr.Outer(), emitter, m_currFunc->tempSlot);
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(m_currFunc->tempSlot);
                    emitter.EmitUint16(fieldOff);
                    emitter.EmitUint16(m_currFunc->tempSlot2);
                }
            }
        }
        return;
    }

    //Subscript assignment: arr[index] = value
    //Reference: EN's AssignStmt::Compile pattern for indexed stores.
    if (kind == NK_SubscriptAssignStmt) {
        auto& sub = static_cast<SnSubscriptAssignStmt&>(stmt);
        //Detect element type from the array's resolved field.
        //The element type is needed for struct deep-copy on write.
        SnField* elemType = nullptr;
        if (sub.Array()->Kind() == NK_IdentifierExpr) {
            auto* arrField = static_cast<SnIdentifierExpr&>(
                *sub.Array()).Field();
            if (arrField && arrField->IsArrayType()
                && arrField->EvalDataType())
                elemType = arrField->EvalDataType();
        }
        //Evaluate value to tempSlot2 first (avoids clobbering by array ref eval)
        EmitExpression(*sub.Value(), emitter, m_currFunc->tempSlot2);
        //Evaluate array reference to tempSlot
        EmitExpression(*sub.Array(), emitter, m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(m_currFunc->tempSlot);
        //Evaluate index to callParamBase (avoids tempSlot/tempSlot2)
        uint16_t indexSlot = m_currFunc->callParamBase;
        EmitExpression(*sub.Index(), emitter, indexSlot);
        //For struct element types, deep-copy value before storing.
        if (elemType && RuntimeTypeKind(elemType) == RTK_Struct) {
            int structIdx = m_compiledModule.FindStruct(elemType->Name());
            emitter.Emit(OpCode::OP_CopyStruct);
            emitter.EmitUint16(m_currFunc->tempSlot2);
            emitter.EmitUint16(m_currFunc->tempSlot2);
            emitter.EmitUint16(structIdx >= 0
                ? static_cast<uint16_t>(structIdx) : 0);
        }
        emitter.Emit(OpCode::OP_StoreElement);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.EmitUint16(indexSlot);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        return;
    }

    //If/else statement.
    //Reference: EN's IfStmt::Compile (SeStatements.cpp:367).
    if (kind == NK_IfStmt) {
        auto& ifStmt = static_cast<SnIfStmt&>(stmt);
        //Evaluate condition to tempSlot
        EmitExpression(*ifStmt.Cond(), emitter, m_currFunc->tempSlot);
        //JumpIfNot to else/endif
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToElse = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder for target
        emitter.EmitUint16(m_currFunc->tempSlot);  //local offset to check
        //Then branch
        EmitStatement(*ifStmt.ThenStmt(), emitter);
        //Jump to endif (skip else branch)
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder for target
        //Else branch
        size_t elseStart = emitter.CurrentOffset();
        if (ifStmt.ElseStmt())
            EmitStatement(*ifStmt.ElseStmt(), emitter);
        //Fixup jumps
        size_t endPos = emitter.CurrentOffset();
        emitter.PatchUint16(jumpToElse, static_cast<uint16_t>(elseStart));
        emitter.PatchUint16(jumpToEnd, static_cast<uint16_t>(endPos));
        return;
    }

    //While loop statement.
    //Reference: EN's WhileStmt::DoCompile (SeStatements.cpp:468).
    if (kind == NK_WhileStmt) {
        auto& whileStmt = static_cast<SnWhileStmt&>(stmt);
        size_t loopStart = emitter.CurrentOffset();

        m_loopStack.push_back(LoopContext{});

        //Evaluate condition to tempSlot
        EmitExpression(*whileStmt.Cond(), emitter, m_currFunc->tempSlot);
        //JumpIfNot to end of loop
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder for target
        emitter.EmitUint16(m_currFunc->tempSlot);  //local offset to check
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //Loop body
        EmitStatement(*whileStmt.Body(), emitter);

        //Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //Fixup jumps
        size_t loopEnd = emitter.CurrentOffset();
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        //Reference: EN's WhileStmt continue jumps back to locStart (condition check)
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopStart));

        m_loopStack.pop_back();
        return;
    }

    //Do-while loop statement.
    //Reference: EN's DoStmt::DoCompile (SeStatements.cpp:507).
    if (kind == NK_DoStmt) {
        auto& doStmt = static_cast<SnDoStmt&>(stmt);

        m_loopStack.push_back(LoopContext{});

        //1. Loop body (executed at least once)
        size_t loopStart = emitter.CurrentOffset();
        EmitStatement(*doStmt.Body(), emitter);

        //2. Continue target: condition check
        size_t continueTarget = emitter.CurrentOffset();

        //3. Condition check
        EmitExpression(*doStmt.Cond(), emitter, m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        emitter.EmitUint16(m_currFunc->tempSlot);
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //4. Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //5. Loop end, fixup jumps
        size_t loopEnd = emitter.CurrentOffset();
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        //Reference: EN's DoStmt continue jumps to locCondition
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));

        m_loopStack.pop_back();
        return;
    }

    //For loop statement.
    //Reference: EN's ForStmt::DoCompile (SeStatements.cpp:546).
    if (kind == NK_ForStmt) {
        auto& forStmt = static_cast<SnForStmt&>(stmt);

        //1. Compile init part (before loop context)
        if (forStmt.Init())
            EmitStatement(*forStmt.Init(), emitter);
        //Compile decomposed init AssignStmts
        for (auto* pExtra : forStmt.InitExtras())
            EmitStatement(*pExtra, emitter);

        //2. Loop start
        size_t loopStart = emitter.CurrentOffset();

        //3. Enter loop context (reference: EN's LoopStmt::Compile)
        m_loopStack.push_back(LoopContext{});

        //4. Condition check
        EmitExpression(*forStmt.Cond(), emitter, m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        emitter.EmitUint16(m_currFunc->tempSlot);
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //5. Loop body
        EmitStatement(*forStmt.Body(), emitter);

        //6. Continue target: fini part
        size_t continueTarget = emitter.CurrentOffset();

        //7. Compile fini part
        if (forStmt.Fini())
            EmitStatement(*forStmt.Fini(), emitter);

        //8. Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //9. Loop end
        size_t loopEnd = emitter.CurrentOffset();

        //10. Fixup jumps (reference: EN's FixDirectJumps)
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));

        m_loopStack.pop_back();
        return;
    }

    //Break statement.
    //Reference: EN's BreakStmt::Compile (SeStatements.cpp:860).
    //Break exits the innermost enclosing switch or loop.
    if (kind == NK_BreakStmt) {
        if (m_loopStack.empty()) {
            //This should be caught by an earlier validation pass.
            //Reference: EN's BreakStmt::Compile checks NestBreaks.
            assert(!"break statement not in loop or switch");
            return;
        }
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpPos = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        m_loopStack.back().breakJumps.push_back(jumpPos);
        return;
    }

    //Continue statement.
    //Reference: EN's ContinueStmt::Compile (SeStatements.cpp:886).
    //Continue targets the innermost enclosing *loop*, not switch.
    if (kind == NK_ContinueStmt) {
        //Walk the stack to find the nearest actual loop (not switch).
        //Reference: EN's ContinueStmt skips switch contexts.
        auto it = m_loopStack.rbegin();
        while (it != m_loopStack.rend() && it->isSwitch)
            ++it;
        if (it == m_loopStack.rend()) {
            //This should be caught by an earlier validation pass.
            //Reference: EN's ContinueStmt::Compile checks NestContinues.
            assert(!"continue statement not in loop");
            return;
        }
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpPos = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        it->continueJumps.push_back(jumpPos);
        return;
    }

    //Switch statement.
    //Reference: EN's SwitchStmt::DoCompile (SeStatements.cpp:725).
    if (kind == NK_SwitchStmt) {
        auto& switchStmt = static_cast<SnSwitchStmt&>(stmt);

        //1. Allocate a dedicated slot for the switch value.
        //This slot must not be overwritten by case condition compilation.
        uint16_t switchSlot = m_currFunc->nextOffset;
        m_currFunc->nextOffset += VALUE_SIZE;

        //2. Compile the switch expression to switchSlot
        EmitExpression(*switchStmt.Cond(), emitter, switchSlot);

        //3. Emit OP_Switch with the switch value's local offset.
        //Note: OP_Switch is a marker opcode (no runtime effect beyond reading
        //the operand). It aids disassembly and could be given runtime semantics
        //in a future optimization (e.g. jump-table dispatch).
        emitter.Emit(OpCode::OP_Switch);
        emitter.EmitUint16(switchSlot);

        //4. Enter switch context (break jumps out of switch)
        m_loopStack.push_back(LoopContext{});
        m_loopStack.back().isSwitch = true;

        //5. Compile each case clause
        //Reference: EN's SwitchStmt::DoCompile — for each case, emit
        //I_Base_Case + jump-to-next-handler placeholder + condition + body.
        std::vector<size_t> nextJumps;
        std::vector<size_t> caseStartOffsets;

        for (auto* pCase : switchStmt.Cases()) {
            //Record this case's start offset
            size_t caseStart = emitter.CurrentOffset();
            caseStartOffsets.push_back(caseStart);

            //Emit OP_Case with jump-to-next-handler placeholder.
            //Note: OP_Case is a marker opcode. Its uint16 operand is patched by
            //FixChainedJumps but never used at runtime (branching is done by
            //OP_JumpIfNot). A future optimization could merge OP_Case with the
            //condition check into a single opcode.
            emitter.Emit(OpCode::OP_Case);
            size_t jumpToNext = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder, patched by FixChainedJumps
            nextJumps.push_back(jumpToNext);

            //Compile condition: switch_value == case_constant
            //Load switch value from dedicated slot to tempSlot2
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(switchSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->tempSlot2);
            //Compile case constant to tempSlot
            EmitExpression(*pCase->Cond(), emitter, m_currFunc->tempSlot);
            //Compare: tempSlot2 == tempSlot → result in tempSlot2
            emitter.Emit(OpCode::OP_Equal_i32);
            emitter.EmitUint16(m_currFunc->tempSlot2);
            emitter.EmitUint16(m_currFunc->tempSlot);
            //If not equal, jump to next case handler
            emitter.Emit(OpCode::OP_JumpIfNot);
            size_t condJumpPos = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder, same target as nextJump
            emitter.EmitUint16(m_currFunc->tempSlot2);
            nextJumps.push_back(condJumpPos);

            //Compile case body
            EmitStatement(*pCase->Body(), emitter);
        }

        //5. Mark locCaseEnd (after all cases, before default)
        size_t locCaseEnd = emitter.CurrentOffset();

        //6. Compile default clause
        if (switchStmt.Default())
            EmitStatement(*switchStmt.Default(), emitter);

        //7. Mark locEnd (after default)
        size_t locEnd = emitter.CurrentOffset();

        //8. FixChainedJumps: patch nextJumps so each case's jumps point
        //to the next case's start. The last case's jumps point to
        //default (if present) or switch end.
        //Reference: EN's Compiler::FixChainedJumps (Compiler.h:86).
        {
            size_t caseCount = caseStartOffsets.size();
            for (size_t i = 0; i < caseCount; ++i) {
                uint16_t target;
                if (i + 1 < caseCount)
                    target = static_cast<uint16_t>(caseStartOffsets[i + 1]);
                else
                    target = static_cast<uint16_t>(
                        switchStmt.Default() ? locCaseEnd : locEnd);

                //Each case has 2 entries in nextJumps: jumpToNext and condJumpPos
                size_t baseIdx = i * 2;
                emitter.PatchUint16(nextJumps[baseIdx], target);
                emitter.PatchUint16(nextJumps[baseIdx + 1], target);
            }
        }

        //9. Fix break jumps (jump to switch end)
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(locEnd));

        m_loopStack.pop_back();
        return;
    }
}

uint16_t VmBackend::AllocLocal(const std::string& name, uint16_t size,
                                uint8_t typeKind, bool isParam) {
    assert(m_currFunc);
    auto it = m_currFunc->localOffsets.find(name);
    if (it != m_currFunc->localOffsets.end())
        return it->second;  // reuse existing slot (flat frame, no block scoping)

    uint16_t offset = m_currFunc->nextOffset;
    m_currFunc->localOffsets[name] = offset;
    m_currFunc->nextOffset += size;

    LocalDescriptor desc;
    desc.offset = offset;
    desc.size = size;
    desc.typeKind = typeKind;
    desc.isParam = isParam ? 1 : 0;
    desc.name = name;
    m_currFunc->func->locals.push_back(std::move(desc));

    return offset;
}

uint16_t VmBackend::FindLocal(const std::string& name) const {
    assert(m_currFunc);
    auto it = m_currFunc->localOffsets.find(name);
    if (it != m_currFunc->localOffsets.end())
        return it->second;
    throw std::runtime_error("NLang backend: local variable not found: " + name);
}

bool VmBackend::SaveModule(BuildEnvironment& env) {
    std::string sFilePath;
    namespace bf = std::filesystem;
    const BuildParams& setting = env.Params();
    const std::string sFileName = setting.m_sOutputModule + ".nmod";

    bf::path modulePath;
    if (setting.m_sOutputDir.empty()) {
        modulePath = bf::current_path() / sFileName;
    } else {
        modulePath = bf::path(setting.m_sOutputDir);
        if (!bf::exists(modulePath)) {
            bf::create_directory(modulePath);
        }
        modulePath /= sFileName;
    }
    sFilePath = modulePath.string();

    env.Log(CLL_Info, "Output module file: %s ...", sFilePath.c_str());

    std::ofstream fs(sFilePath, std::ios::binary);
    if (!fs.is_open()) {
        env.Log(CLL_Fatal, "Failed to open file: %s.", sFilePath.c_str());
        return false;
    }

    // Magic
    const char magic[] = "NLANGMOD";
    fs.write(magic, 8);

    // Version
    uint16_t majorVer = 1, minorVer = 0;
    fs.write(reinterpret_cast<const char*>(&majorVer), sizeof(majorVer));
    fs.write(reinterpret_cast<const char*>(&minorVer), sizeof(minorVer));

    // Module name
    uint32_t nameLen = static_cast<uint32_t>(m_compiledModule.name.size());
    fs.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
    fs.write(m_compiledModule.name.c_str(), nameLen);

    // String constants
    uint32_t strCount = static_cast<uint32_t>(
        m_compiledModule.stringConstants.size());
    fs.write(reinterpret_cast<const char*>(&strCount), sizeof(strCount));
    for (auto& s : m_compiledModule.stringConstants) {
        uint32_t len = static_cast<uint32_t>(s.size());
        fs.write(reinterpret_cast<const char*>(&len), sizeof(len));
        fs.write(s.c_str(), len);
    }

    // Functions
    uint32_t funcCount = static_cast<uint32_t>(
        m_compiledModule.functions.size());
    fs.write(reinterpret_cast<const char*>(&funcCount), sizeof(funcCount));

    for (auto& func : m_compiledModule.functions) {
        uint32_t fnameLen = static_cast<uint32_t>(func.name.size());
        fs.write(reinterpret_cast<const char*>(&fnameLen), sizeof(fnameLen));
        fs.write(func.name.c_str(), fnameLen);

        fs.write(reinterpret_cast<const char*>(&func.localsSize),
                 sizeof(func.localsSize));
        fs.write(reinterpret_cast<const char*>(&func.paramCount),
                 sizeof(func.paramCount));
        fs.write(reinterpret_cast<const char*>(&func.returnTypeKind),
                 sizeof(func.returnTypeKind));

        uint32_t bcSize = static_cast<uint32_t>(func.bytecode.size());
        fs.write(reinterpret_cast<const char*>(&bcSize), sizeof(bcSize));
        if (bcSize > 0)
            fs.write(reinterpret_cast<const char*>(func.bytecode.data()),
                     bcSize);
    }

    // Struct descriptors
    uint32_t structCount = static_cast<uint32_t>(
        m_compiledModule.structs.size());
    fs.write(reinterpret_cast<const char*>(&structCount), sizeof(structCount));

    for (auto& st : m_compiledModule.structs) {
        uint32_t stNameLen = static_cast<uint32_t>(st.name.size());
        fs.write(reinterpret_cast<const char*>(&stNameLen), sizeof(stNameLen));
        fs.write(st.name.c_str(), stNameLen);

        fs.write(reinterpret_cast<const char*>(&st.fieldCount),
                 sizeof(st.fieldCount));

        //Field names
        for (size_t i = 0; i < st.fieldCount; ++i) {
            uint32_t fnLen = static_cast<uint32_t>(st.fieldNames[i].size());
            fs.write(reinterpret_cast<const char*>(&fnLen), sizeof(fnLen));
            fs.write(st.fieldNames[i].c_str(), fnLen);
        }

        //Field type kinds
        for (size_t i = 0; i < st.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&st.fieldTypeKinds[i]),
                     sizeof(st.fieldTypeKinds[i]));
        }

        //Field struct indices
        for (size_t i = 0; i < st.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&st.fieldStructIndices[i]),
                     sizeof(st.fieldStructIndices[i]));
        }

        //Field class indices
        for (size_t i = 0; i < st.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&st.fieldClassIndices[i]),
                     sizeof(st.fieldClassIndices[i]));
        }
    }

    // Class descriptors
    uint32_t classCount = static_cast<uint32_t>(
        m_compiledModule.classes.size());
    fs.write(reinterpret_cast<const char*>(&classCount), sizeof(classCount));

    for (auto& cc : m_compiledModule.classes) {
        uint32_t nameLen = static_cast<uint32_t>(cc.name.size());
        fs.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        fs.write(cc.name.c_str(), nameLen);

        fs.write(reinterpret_cast<const char*>(&cc.fieldCount),
                 sizeof(cc.fieldCount));
        fs.write(reinterpret_cast<const char*>(&cc.superClassIdx),
                 sizeof(cc.superClassIdx));

        //Field names
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            uint32_t fnLen = static_cast<uint32_t>(cc.fieldNames[i].size());
            fs.write(reinterpret_cast<const char*>(&fnLen), sizeof(fnLen));
            fs.write(cc.fieldNames[i].c_str(), fnLen);
        }

        //Field type kinds
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.fieldTypeKinds[i]),
                     sizeof(cc.fieldTypeKinds[i]));
        }

        //Field struct indices
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.fieldStructIndices[i]),
                     sizeof(cc.fieldStructIndices[i]));
        }

        //Field class indices
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.fieldClassIndices[i]),
                     sizeof(cc.fieldClassIndices[i]));
        }

        //Field access
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.fieldAccess[i]),
                     sizeof(cc.fieldAccess[i]));
        }

        //Method indices
        uint16_t methodCount = static_cast<uint16_t>(cc.methodIndices.size());
        fs.write(reinterpret_cast<const char*>(&methodCount), sizeof(methodCount));
        for (size_t i = 0; i < cc.methodIndices.size(); ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.methodIndices[i]),
                     sizeof(cc.methodIndices[i]));
        }

        //Constructor index
        fs.write(reinterpret_cast<const char*>(&cc.constructorIdx),
                 sizeof(cc.constructorIdx));
    }

    //Array type descriptors
    uint32_t arrayTypeCount = static_cast<uint32_t>(
        m_compiledModule.arrayTypes.size());
    fs.write(reinterpret_cast<const char*>(&arrayTypeCount),
             sizeof(arrayTypeCount));
    for (auto& at : m_compiledModule.arrayTypes) {
        fs.write(reinterpret_cast<const char*>(&at.elemKind),
                 sizeof(at.elemKind));
        fs.write(reinterpret_cast<const char*>(&at.elemTypeIdx),
                 sizeof(at.elemTypeIdx));
    }

    fs.close();
    return true;
}

} // namespace nlang
