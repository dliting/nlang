/*---
    EmitStaticInit.cpp — v0.12.6 lazy static initialization, backend
    side (spec §6.2 D2/D4/D10; Task 7 D11 adds the interface flavor).
    One file owns both halves:

    Trigger gates — EmitClassInitGate/EmitInterfaceInitGate prefix
    every first-active-use site (static field read/store/compound,
    static method call, construction) with gate+call. An owner with no
    initializer emits nothing anywhere (D9's zero machinery); the
    gate's donePc skips just the paired call, so an initialized owner
    pays two instructions per site after the first.

    The initializer itself — GenerateStaticInitFunction emits the
    synthesized "$staticInit" member with the byte-level protocol glue
    the AST cannot spell: super gate+call INSIDE the try (a super
    failure must reset this class's owner too, D10; interfaces have no
    chain), then the field assignments as ordinary statements, then a
    catch-all handler that resets the owner and rethrows, then Done +
    return.
---*/
#include "VmBackend.h"
#include "builder/BuiltinNames.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>

namespace nlang {

//The owner's synthesized initializer member, or null when it needs no
//initialization (no initialized static of its own or in its chain —
//such owners were never given one by the resolver synthesis).
template <typename DeclT>
static SnFunction* StaticInitMemberOf(DeclT& decl)
{
    for (auto& member : decl.Members())
        if (member.Kind() == NK_Function
            && IsStaticInitName(member.Name()))
            return static_cast<SnFunction*>(&member);
    return nullptr;
}

//Prefix a first-active-use site with the class's init gate. No
//synthesized member → no code (initializer-less classes keep their
//zero cost, and cross-unit stubs carry none until the import task).
void VmBackend::EmitClassInitGate(SnClassDecl& decl,
                                  BytecodeEmitter& emitter)
{
    SnFunction* init = StaticInitMemberOf(decl);
    if (!init)
        return;
    emitter.Emit(OpCode::OP_ClassInitGate);
    emitter.EmitUint16(static_cast<uint16_t>(ClassSlotFor(decl)));
    const size_t donePatch = emitter.CurrentOffset();
    emitter.EmitUint16(0);   //donePc placeholder, absolute (OpJump idiom)
    emitter.EmitUint16(0);   //kind: class
    emitter.Emit(OpCode::OP_CallFunc);
    emitter.EmitUint16(
        static_cast<uint16_t>(FunctionSlotFor(*init)));
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.PatchUint16(donePatch, emitter.CurrentOffset());
}

//Task 7 (D11): the interface flavor — kind 1, the interface-table
//index, same gate+call shape. Reading a runtime interface constant or
//calling an interface static method triggers it; implements does not.
void VmBackend::EmitInterfaceInitGate(SnInterfaceDecl& decl,
                                      BytecodeEmitter& emitter)
{
    SnFunction* init = StaticInitMemberOf(decl);
    if (!init)
        return;
    emitter.Emit(OpCode::OP_ClassInitGate);
    emitter.EmitUint16(InterfaceSlotFor(decl));
    const size_t donePatch = emitter.CurrentOffset();
    emitter.EmitUint16(0);   //donePc placeholder, absolute (OpJump idiom)
    emitter.EmitUint16(1);   //kind: interface
    emitter.Emit(OpCode::OP_CallFunc);
    emitter.EmitUint16(
        static_cast<uint16_t>(FunctionSlotFor(*init)));
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.PatchUint16(donePatch, emitter.CurrentOffset());
}

//Gate the declaring owner of a static member, when one is bound. The
//resolver binds inherited statics to the DECLARING class's member, so
//the gate targets exactly the class whose initializer must run (D4);
//an interface member routes to the interface gate (D11).
void VmBackend::EmitStaticFieldGate(SnField* field,
                                    BytecodeEmitter& emitter)
{
    if (!field || !field->ContainFlags(NF_Static))
        return;
    auto* parent = field->Parent();
    if (parent && parent->Kind() == NK_ClassDecl)
        EmitClassInitGate(static_cast<SnClassDecl&>(*parent), emitter);
    else if (parent && parent->Kind() == NK_InterfaceDecl)
        EmitInterfaceInitGate(
            static_cast<SnInterfaceDecl&>(*parent), emitter);
}

//GenerateFunction's static-init arm: same record-out-of-table and
//frame-layout discipline, with the protocol glue body instead of
//EmitBodyAndImplicitReturn. m_pCurrClass is the owning class (the
//recursive walk drives this from the class's member loop); an
//interface's $staticInit runs with m_pCurrInterface instead (Task 7).
void VmBackend::GenerateStaticInitFunction(SnFunction& func,
                                           size_t funcIdx)
{
    NLANG_ASSERT(m_pCurrClass || m_pCurrInterface);
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
    if (m_pCurrInterface)
        EmitInterfaceStaticInitBody(*m_pCurrInterface, func, emitter);
    else
        EmitStaticInitBody(*m_pCurrClass, func, emitter);

    compiledFunc.bytecode = emitter.TakeBytes();
    compiledFunc.localsSize = ctx.nextOffset;

    CheckEvalAreaWalkerDrift(func, ctx, stats);
    BuildEvalAreaHandleMask(compiledFunc, ctx);

    m_currFunc = nullptr;
    m_compiledModule.functions[funcIdx] = std::move(compiledFunc);
}

//The "$staticInit" protocol body shared by both owner kinds:
//[optional super gate (classes); assignments] inside a catch-all try
//whose handler resets the owner (D10) and rethrows; Done + return
//outside. The super gate sits INSIDE the try — when the super
//initializer fails, the unwinding reaches this frame's handler too,
//so this class's owner state resets instead of hanging on a dead
//attempt. Statements are the resolver-synthesized assignments; they
//emit through the ordinary statement machinery.
void VmBackend::EmitStaticInitProtocolBody(uint16_t ownerIdx,
                                           uint16_t kind,
                                           SnFunction& func,
                                           SnClassDecl* super,
                                           BytecodeEmitter& emitter)
{
    const uint16_t tryStart =
        static_cast<uint16_t>(emitter.CurrentOffset());
    if (super)
        EmitClassInitGate(*super, emitter);
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
    emitter.EmitUint16(ownerIdx);
    emitter.EmitUint16(kind);
    emitter.Emit(OpCode::OP_Rethrow);

    emitter.PatchUint16(endPatch,
                        static_cast<uint16_t>(emitter.CurrentOffset()));
    emitter.Emit(OpCode::OP_ClassInitDone);
    emitter.EmitUint16(ownerIdx);
    emitter.EmitUint16(kind);
    emitter.Emit(OpCode::OP_Return);
}

void VmBackend::EmitStaticInitBody(SnClassDecl& decl, SnFunction& func,
                                   BytecodeEmitter& emitter)
{
    EmitStaticInitProtocolBody(
        static_cast<uint16_t>(ClassSlotFor(decl)), 0, func,
        decl.SuperClass(), emitter);
}

void VmBackend::EmitInterfaceStaticInitBody(SnInterfaceDecl& decl,
                                            SnFunction& func,
                                            BytecodeEmitter& emitter)
{
    //Interfaces have no inheritance chain — no super gate.
    EmitStaticInitProtocolBody(InterfaceSlotFor(decl), 1, func,
                               nullptr, emitter);
}

//Task 7 (spec §8): the declaration-site once-guard of a static local —
//the protocol shape of EmitStaticInitProtocolBody folded into the
//declaring function, keyed on the slot's own machine:
//  SlotInitGate slot, done   (Done / re-entrant-self seek `done`)
//  try [ initializer rhs -> claim; StoreStatic slot, claim ]
//  SlotInitDone slot; Jump done
//  handler: SlotInitFail slot; Rethrow
//  done: (the statements after the declaration)
//The RHS claim mirrors the class store arm; the assign walker's
//identifier-target claim(1) covers it. Flipping hasInitializer here
//keeps the record and the guard in one place — an initializer-less
//static (no decomposed assign) keeps D9's read-zero machinery.
void VmBackend::EmitStaticLocalInitSequence(SnAssignStmt& assign,
    uint16_t slotIdx, BytecodeEmitter& emitter)
{
    m_compiledModule.staticSlots[slotIdx].hasInitializer = 1;
    emitter.Emit(OpCode::OP_SlotInitGate);
    emitter.EmitUint16(slotIdx);
    const size_t donePatch = emitter.CurrentOffset();
    emitter.EmitUint16(0);   //donePc placeholder, absolute (OpJump idiom)

    const uint16_t tryStart =
        static_cast<uint16_t>(emitter.CurrentOffset());
    {
        EvalAreaClaim claim(*this, 1);
        uint16_t valueSlot = claim.base();
        EmitExpression(*assign.Right(), emitter, valueSlot);
        emitter.Emit(OpCode::OP_StoreStatic);
        emitter.EmitUint16(slotIdx);
        emitter.EmitUint16(valueSlot);
    }
    const uint16_t tryEnd =
        static_cast<uint16_t>(emitter.CurrentOffset());

    emitter.Emit(OpCode::OP_SlotInitDone);
    emitter.EmitUint16(slotIdx);
    emitter.Emit(OpCode::OP_Jump);
    const size_t endPatch = emitter.CurrentOffset();
    emitter.EmitUint16(0);

    //Handler entry mirrors the protocol body: the runtime writes the
    //caught exception heap idx into the catch local unconditionally,
    //so the slot must be real even though OP_Rethrow never reads it.
    //AllocLocal dedupes by name — every guard in this function shares
    //one exc local, never live at the same time.
    const uint16_t handlerPc =
        static_cast<uint16_t>(emitter.CurrentOffset());
    const uint16_t excOff = AllocLocal("$slot_exc", kFrameSlotBytes,
                                       RTK_Class, false);
    m_currFunc->func->tryBlocks.push_back(
        {tryStart, tryEnd, handlerPc, 0xFFFF, excOff});
    emitter.Emit(OpCode::OP_SlotInitFail);
    emitter.EmitUint16(slotIdx);
    emitter.Emit(OpCode::OP_Rethrow);

    const uint16_t donePc =
        static_cast<uint16_t>(emitter.CurrentOffset());
    emitter.PatchUint16(donePatch, donePc);
    emitter.PatchUint16(endPatch, donePc);
}

} //namespace nlang
