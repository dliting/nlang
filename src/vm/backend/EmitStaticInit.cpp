/*---
    EmitStaticInit.cpp — v0.12.6 lazy static initialization, backend
    side (spec §6.2 D2/D4/D10). One file owns both halves:

    Trigger gates — EmitClassInitGate prefixes every first-active-use
    site (static field read/store/compound, static method call,
    construction) with gate+call. A class with no initializer emits
    nothing anywhere (D9's zero machinery); the gate's donePc skips
    just the paired call, so an initialized class pays two instructions
    per site after the first.

    The initializer itself — GenerateStaticInitFunction emits the
    synthesized "$staticInit" member with the byte-level protocol glue
    the AST cannot spell: super gate+call INSIDE the try (a super
    failure must reset this class's owner too, D10), then the field
    assignments as ordinary statements, then a catch-all handler that
    resets the owner and rethrows, then Done + return.
---*/
#include "VmBackend.h"
#include "builder/BuiltinNames.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>

namespace nlang {

//The class's synthesized initializer member, or null when the class
//needs no initialization (no initialized static of its own or in its
//chain — such classes were never given one by the resolver synthesis).
static SnFunction* StaticInitMemberOf(SnClassDecl& decl)
{
    for (auto& member : decl.Members())
        if (member.Kind() == NK_Function
            && IsStaticInitName(member.Name()))
            return static_cast<SnFunction*>(&member);
    return nullptr;
}

//The declaring class of a static member: the owning class node. The
//resolver binds inherited statics to the DECLARING class's member, so
//the gate targets exactly the class whose initializer must run (D4).
static SnClassDecl* DeclaringClassOf(SnField& member)
{
    auto* parent = member.Parent();
    if (parent && parent->Kind() == NK_ClassDecl)
        return static_cast<SnClassDecl*>(parent);
    return nullptr;
}

//Prefix a first-active-use site with the init gate. No synthesized
//member → no code (initializer-less classes keep their zero cost, and
//cross-unit stubs carry none until the import task). The gate's
//donePc lands right after the paired OP_CallFunc: Done-phase walks
//skip exactly the call.
void VmBackend::EmitClassInitGate(SnClassDecl& decl,
                                  BytecodeEmitter& emitter)
{
    SnFunction* init = StaticInitMemberOf(decl);
    if (!init)
        return;
    const uint16_t classIdx =
        static_cast<uint16_t>(ClassSlotFor(decl));
    emitter.Emit(OpCode::OP_ClassInitGate);
    emitter.EmitUint16(classIdx);
    const size_t donePatch = emitter.CurrentOffset();
    emitter.EmitUint16(0);   //donePc placeholder, absolute (OpJump idiom)
    emitter.EmitUint16(0);   //kind: class (interface reserved)
    emitter.Emit(OpCode::OP_CallFunc);
    emitter.EmitUint16(
        static_cast<uint16_t>(FunctionSlotFor(*init)));
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.PatchUint16(donePatch, emitter.CurrentOffset());
}

//Gate the declaring class of a static field, when one is bound.
void VmBackend::EmitStaticFieldGate(SnField* field,
                                    BytecodeEmitter& emitter)
{
    if (!field || !field->ContainFlags(NF_Static))
        return;
    if (auto* owner = DeclaringClassOf(*field))
        EmitClassInitGate(*owner, emitter);
}

//GenerateFunction's static-init arm: same record-out-of-table and
//frame-layout discipline, with the protocol glue body instead of
//EmitBodyAndImplicitReturn. m_pCurrClass is the owning class (the
//recursive walk drives this from the class's member loop).
void VmBackend::GenerateStaticInitFunction(SnFunction& func,
                                           size_t funcIdx)
{
    NLANG_ASSERT(m_pCurrClass);
    CompiledFunction compiledFunc =
        std::move(m_compiledModule.functions[funcIdx]);

    CollectSignatureTypeDescs(func, compiledFunc);

    FuncContext ctx;
    ctx.func = &compiledFunc;
    ctx.nextOffset = 0;
    m_currFunc = &ctx;
    m_pCurrEmitter = nullptr;

    AllocParamsAndDefaults(func, ctx, compiledFunc);
    CallSlotStats stats = ReserveReturnAndCallSlots(func, ctx,
                                                    compiledFunc);

    BytecodeEmitter emitter;
    EmitStaticInitBody(*m_pCurrClass, func, emitter);

    compiledFunc.bytecode = emitter.TakeBytes();
    compiledFunc.localsSize = ctx.nextOffset;

    CheckEvalAreaWalkerDrift(func, ctx, stats);
    BuildEvalAreaHandleMask(compiledFunc, ctx);

    m_currFunc = nullptr;
    m_compiledModule.functions[funcIdx] = std::move(compiledFunc);
}

//The "$staticInit" body: [super gate+call; assignments] inside a
//catch-all try whose handler resets the owner (D10) and rethrows;
//Done + return outside. The super gate sits INSIDE the try — when the
//super initializer fails, the unwinding reaches this frame's handler
//too, so this class's owner state resets instead of hanging on a dead
//attempt. Statements are the resolver-synthesized assignments; they
//emit through the ordinary statement machinery.
void VmBackend::EmitStaticInitBody(SnClassDecl& decl, SnFunction& func,
                                   BytecodeEmitter& emitter)
{
    const uint16_t classIdx =
        static_cast<uint16_t>(ClassSlotFor(decl));
    const uint16_t tryStart =
        static_cast<uint16_t>(emitter.CurrentOffset());
    if (decl.SuperClass())
        EmitClassInitGate(*decl.SuperClass(), emitter);
    for (auto& stmt : func.Body()->Statements())
        EmitStatement(stmt, emitter);
    const uint16_t tryEnd =
        static_cast<uint16_t>(emitter.CurrentOffset());

    emitter.Emit(OpCode::OP_Jump);
    const size_t endPatch = emitter.CurrentOffset();
    emitter.EmitUint16(0);

    //Handler entry: the runtime writes the caught exception heap idx
    //into the catch local unconditionally (finally arm's contract), so
    //the slot must be real even though OP_Rethrow never reads it.
    const uint16_t handlerPc =
        static_cast<uint16_t>(emitter.CurrentOffset());
    const uint16_t excOff = AllocLocal("$init_exc", kFrameSlotBytes,
                                       RTK_Class, false);
    m_currFunc->func->tryBlocks.push_back(
        {tryStart, tryEnd, handlerPc, 0xFFFF, excOff});
    emitter.Emit(OpCode::OP_ClassInitFail);
    emitter.EmitUint16(classIdx);
    emitter.EmitUint16(0);   //kind: class
    emitter.Emit(OpCode::OP_Rethrow);

    emitter.PatchUint16(endPatch,
                        static_cast<uint16_t>(emitter.CurrentOffset()));
    emitter.Emit(OpCode::OP_ClassInitDone);
    emitter.EmitUint16(classIdx);
    emitter.EmitUint16(0);   //kind: class
    emitter.Emit(OpCode::OP_Return);
}

} //namespace nlang
