#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/runtime/Module.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <iostream>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

VmBackend::VmBackend() = default;
VmBackend::~VmBackend() = default;

void VmBackend::OnModuleCreate(Module& module) {
    m_compiledModule.name = module.Name().ToString();
}

void VmBackend::GenerateTypes(SnNamespace& root) {
    // Phase 2: no type layout needed beyond basic types
}

void VmBackend::GenerateData(SnNamespace& root) {
    // Phase 2: no data layout needed beyond function registration
}

void VmBackend::GenerateStatements(SnNamespace& root) {
    // Phase 1: Register all function names so FindFunction works for
    // forward references and recursion.
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(member);
            if (!func.Body())
                continue;
            CompiledFunction cf;
            cf.name = func.Name();
            m_compiledModule.functions.push_back(std::move(cf));
        } else if (CanBeFuncParent(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_Function) {
                    auto& func = static_cast<SnFunction&>(child);
                    if (!func.Body())
                        continue;
                    CompiledFunction cf;
                    cf.name = func.Name();
                    m_compiledModule.functions.push_back(std::move(cf));
                }
            }
        }
    }

    // Phase 2: Generate bytecode for each function.
    size_t funcIdx = 0;
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(member);
            if (!func.Body())
                continue;
            GenerateFunction(func, funcIdx);
            ++funcIdx;
        } else if (CanBeFuncParent(member.Kind())) {
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

void VmBackend::GenerateFunction(SnFunction& func, size_t funcIdx) {
    CompiledFunction& compiledFunc = m_compiledModule.functions[funcIdx];

    FuncContext ctx;
    ctx.func = &compiledFunc;
    ctx.nextOffset = 0;
    m_currFunc = &ctx;

    // Allocate slots for parameters
    for (auto& param : func.Params()) {
        AllocLocal(param.Name(), VALUE_SIZE,
                   static_cast<uint8_t>(param.EvalDataType()
                       ? param.EvalDataType()->Kind() : NK_Int32),
                   true);
    }
    compiledFunc.paramCount = static_cast<uint16_t>(func.Params().size());

    // Return type
    if (func.HasReturn() && func.ReturnType()) {
        auto* retType = func.ReturnType()->Field();
        compiledFunc.returnTypeKind = retType
            ? static_cast<uint16_t>(retType->Kind()) : 0;
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
            emitter.Emit(OpCode::OP_ConstZero);
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
                ++paramIdx;
            }
        }

        // Find function index
        int funcIndex = m_compiledModule.FindFunction(
            callee ? callee->Name() : invoke.CalleeName());
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

    // Member expression - delegate to inner
    if (kind == NK_MemberExpr) {
        auto& member = static_cast<SnMemberExpr&>(expr);
        auto* inner = member.Inner();
        if (inner && inner->Kind() == NK_InvokeExpr) {
            EmitExpression(*static_cast<SnExpression*>(inner), emitter, resultOffset);
        }
        return;
    }

    // Name expression - delegate
    if (kind == NK_NameExpr) {
        auto& nameExpr = static_cast<SnNameExpr&>(expr);
        if (nameExpr.Expr())
            EmitExpression(*nameExpr.Expr(), emitter, resultOffset);
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
        //overwriting the left before the binary op executes. When resultOffset
        //is tempSlot, use tempSlot2 for right; otherwise use tempSlot.
        //This works for nested expressions too: e.g. (a+b)*(c+d) evaluates
        //left subexpr to resultOffset (using tempSlot2 internally), then right
        //subexpr to tempSlot — no conflict because each subexpr uses its own
        //rightSlot derived from its own resultOffset.
        uint16_t rightSlot = (resultOffset == m_currFunc->tempSlot)
            ? m_currFunc->tempSlot2 : m_currFunc->tempSlot;
        EmitExpression(*bin.Left(), emitter, resultOffset);
        EmitExpression(*bin.Right(), emitter, rightSlot);

        auto* evalType = bin.Left()->EvalDataType();
        bool isFloat = evalType && evalType->Kind() == NK_Float;

        switch (op) {
        case SnBinaryExpr::OP_Add:
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
            emitter.Emit(isFloat ? OpCode::OP_Equal_f32 : OpCode::OP_Equal_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_NotEqual:
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
        auto* evalType = decl.Type()->Field();
        uint8_t typeKind = evalType ? static_cast<uint8_t>(evalType->Kind()) : NK_Int32;
        for (auto& local : decl.Decls()) {
            AllocLocal(local.name, VALUE_SIZE, typeKind, false);
        }
        return;
    }

    //Assignment statement.
    //Reference: EN's AssignStmt::Compile (SeStatements.cpp:313).
    if (kind == NK_AssignStmt) {
        auto& assign = static_cast<SnAssignStmt&>(stmt);
        //Left side must be an identifier resolved to a local variable
        if (assign.Left()->Kind() == NK_IdentifierExpr) {
            auto& idExpr = static_cast<SnIdentifierExpr&>(*assign.Left());
            auto* field = idExpr.Field();
            if (field) {
                uint16_t offset = FindLocal(field->Name());
                EmitExpression(*assign.Right(), emitter, offset);
            }
        }
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
    if (kind == NK_BreakStmt) {
        if (m_loopStack.empty()) {
            //Error: break not in loop
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
    if (kind == NK_ContinueStmt) {
        if (m_loopStack.empty()) {
            //Error: continue not in loop
            return;
        }
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpPos = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        m_loopStack.back().continueJumps.push_back(jumpPos);
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

    fs.close();
    return true;
}

} // namespace nlang
