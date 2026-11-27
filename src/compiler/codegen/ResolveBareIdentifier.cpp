/*---
    ResolveBareIdentifier.cpp — bare identifier target classification.
    The single source for what an unqualified name addresses (local / static
    slot / static local slot / implicit this-field), consumed by the
    identifier read, plain assignment and compound assignment emitters.
    Split out of VmBackend.cpp (2026-11-02 source-size guard).
---*/
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include "builder/SymbolSlots.hpp"
#include <nlang/compiler/SnData.h>

#include <stdexcept>

namespace nlang {

//Field-first storage channels of ResolveBareIdentifier: the three ways a
//binding addresses storage that is NOT the flat frame's name-keyed pool
//(a module slot, a captured display field, a declaration-private slot).
//True with t filled when one answers; false leaves the caller on the
//name-keyed local / this-field arms.
bool VmBackend::TryResolveOwnedStorage(SnField* field, BareIdTarget& t) {
    //v0.12.6 Task 7 (spec §8): a static local addresses its module
    //slot. The check is field-first — the binding node decides, so a
    //same-named frame local (flat frames reuse names) can never
    //shadow the slot-backed one or vice versa. SnLocalVar shares
    //NK_FormalParam as its kind; only locals carry NF_Static on it.
    if (field->Kind() == NK_FormalParam && field->ContainFlags(NF_Static)) {
        t.kind = BareIdTarget::StaticSlot;
        t.slotIdx = StaticLocalSlotIndexOf(*field);
        return true;
    }
    //D-1 (0.13.13a): the call-site snapshot arm answers before the
    //NF_Captured one below — see TryResolveSnapshotCaptureField for
    //why the snapshot outranks the DisplayField redirect.
    if (TryResolveSnapshotCaptureField(field, t))
        return true;
    //0.13.10 Task 6: captured storage. Field-first, before the frame
    //locals — flat frames reuse names (AllocLocal name-keying), and the
    //display field IS the storage once NF_Captured is stamped: the
    //declaring frame's coincidental local must never answer.
    if (field->ContainFlags(NF_Captured)) {
        t = DisplayFieldOfCaptured(field);
        NLANG_ASSERT(t.kind == BareIdTarget::DisplayField
            || t.kind == BareIdTarget::CaptureField);
        return true;
    }
    //0.13.10 Task 9: declaration-keyed storage — the third field-first
    //channel. A published descriptor owns its private slot; a shadowed
    //name must never answer another binding's slot (loop head vs outer
    //local, nested heads, catch var, block declarations).
    auto it = m_currFunc->localSlotOfDecl.find(field);
    if (it != m_currFunc->localSlotOfDecl.end()) {
        t.kind = BareIdTarget::Local;
        t.localOffset = it->second;
        return true;
    }
    return false;
}

//D-1 (0.13.13a): call-site snapshot capture, the first arm of
//TryResolveOwnedStorage inside a synthetic frame. The lambda at a
//formal's default-value root snapshotted the host formal onto the
//display it carries — the synthetic frame's local 0 arrives as that
//instance (the prologue's alias arm). The declaration keeps its own
//frame storage and carries no NF_Captured, so the reference is
//answered through the snapshot field's CaptureSource link, hopping
//the "$outer" chain from the frame's anchor class (hop 0 = the
//arrival display). The arm sits BEFORE the NF_Captured one: a
//dual-captured formal's ordinary display is not reachable from this
//frame — its chain starts at the arrival instance — so the reference
//must answer from the snapshot, never from the DisplayField
//redirect. Outside a generated frame the declaration answers from
//its own slot instead: the caller evaluates the default expression
//before the display exists, so those references never see the
//snapshot. A miss on the chain (the declaration is not snapshotted
//on this lambda) falls through untouched.
bool VmBackend::TryResolveSnapshotCaptureField(SnField* field,
    BareIdTarget& t)
{
    if (!InSyntheticCaptureFrame())
        return false;
    auto* pCls = m_pCurrClass;
    for (int hop = 0; pCls; ++hop) {
        SnField* pOuter = nullptr;
        for (auto& member : pCls->Members()) {
            if (member.Kind() != NK_ClassField
                || member.ContainFlags(NF_Static))
                continue;
            auto& snapField = static_cast<SnClassField&>(member);
            if (snapField.CaptureSource() == field) {
                t.kind = BareIdTarget::CaptureField;
                t.outerHops = hop;
                t.owner = pCls;
                t.fieldOff = FindClassFieldOffset(*pCls, member.Name());
                NLANG_ASSERT(t.fieldOff >= 0);
                return true;
            }
            if (member.Name() == "$outer")
                pOuter = &snapField;
        }
        if (!pOuter)
            break;   //chain root without a match
        pCls = dynamic_cast<SnClassDecl*>(pOuter->EvalDataType());
        NLANG_ASSERT(pCls);
    }
    return false;
}

//Walker/dispatch predicate (see the header note): NF_Captured covers
//the ordinary redirect; the published DisplayField pointer
//additionally covers the 0.13.13a D-1 call-site snapshot (the
//snapshot synthesis publishes it first-set-wins, ordinary synthesis
//unconditionally — a dual-captured descriptor answers true through
//either half). Locals and lambda parameters share the NK_FormalParam
//kind, as in CapturedDisplayFieldOf; no other descriptor carries the
//pointer.
bool VmBackend::IsCapturedField(const SnField* pField)
{
    if (!pField)
        return false;
    if (pField->ContainFlags(NF_Captured))
        return true;
    if (auto* pLocal = dynamic_cast<const SnLocalVar*>(pField))
        return pLocal->DisplayField() != nullptr;
    if (auto* pFormal = dynamic_cast<const SnFormalParam*>(pField))
        return pFormal->DisplayField() != nullptr;
    return false;
}

VmBackend::BareIdTarget VmBackend::ResolveBareIdentifier(SnField* field) {
    BareIdTarget t;
    if (!field)
        return t;
    if (TryResolveOwnedStorage(field, t))
        return t;
    auto it = m_currFunc->localOffsets.find(field->Name());
    if (it != m_currFunc->localOffsets.end()) {
        t.kind = BareIdTarget::Local;
        t.localOffset = it->second;
        return t;
    }
    //0.12.6 D1/D3: a static class field is module-slot-backed — no
    //instance involved, so this precedes the this-field walk (whose
    //offset solver skips statics anyway). Locals above keep shadowing.
    if (field->Kind() == NK_ClassField
        && field->ContainFlags(NF_Static)) {
        t.kind = BareIdTarget::StaticSlot;
        t.slotIdx = StaticSlotIndexOf(
            static_cast<SnClassField&>(*field));
        return t;
    }
    ResolveThisFieldTarget(*field, t);
    return t;
}

//ThisField arm of ResolveBareIdentifier: the owning class/struct walk
//with the offset solver (structs flatten, classes walk the chain).
//True with t filled when the field resolves to instance storage.
bool VmBackend::ResolveThisFieldTarget(SnField& field, BareIdTarget& t) {
    auto* pOwner = OwningTypeOfMemberField(&field);
    if (!pOwner)
        return false;
    //0.9.11 D2: struct fields flatten through the struct helper (the
    //class helper walks the inheritance chain, which structs lack).
    const int off = (field.Kind() == NK_StructField)
        ? FindFieldOffset(static_cast<SnStructDecl&>(*pOwner),
                          field.Name())
        : FindClassFieldOffset(static_cast<SnClassDecl&>(*pOwner),
                               field.Name());
    if (off < 0)
        return false;
    t.kind = BareIdTarget::ThisField;
    t.owner = pOwner;
    t.fieldOff = off;
    return true;
}

//v0.12.6 Task 8: own fields answer from the registration map; an
//imported stub field (NF_Imported owner, rider valid) instead creates
//the placeholder import record on first use — StaticSlotSymbolSlot
//dedupes, so every use site in this unit shares one tail slot. The
//rider's absence on an imported field (a folded constant) never
//reaches here: the constant-read phase consumes it first.
//A same-build foreign unit (an inlined library source, or a sibling
//project unit) is no stub and carries no wire rider — the rider is
//replayed from the shadow registration archive, the same pattern the
//function channel uses for cross-unit callees
//(EffectiveProducerSlotIdx).
uint16_t VmBackend::StaticSlotIndexOf(SnClassField& field) {
    auto it = m_staticSlotByField.find(&field);
    if (it != m_staticSlotByField.end())
        return it->second;
    auto* pParent = field.Parent();
    //Class and interface declarations are both SnField subclasses; the
    //SnField& view serves the owner-tag queries below.
    if (pParent
        && (pParent->Kind() == NK_ClassDecl
            || pParent->Kind() == NK_InterfaceDecl)) {
        auto& owner = static_cast<SnField&>(*pParent);
        if (owner.IsImported() || !IsOwnUnit(owner)) {
            const uint16_t rider = EffectiveProducerSlotIdx(field);
            if (rider != 0xFFFF) {
                const uint8_t ownerKind =
                    pParent->Kind() == NK_InterfaceDecl ? 1 : 0;
                const uint16_t slot = static_cast<uint16_t>(
                    StaticSlotSymbolSlot(m_compiledModule,
                        SymbolSlotModulePath(*m_pRegistry, owner),
                        field.Name(), KeyOf(owner), ownerKind, rider));
                m_staticSlotByField[&field] = slot;
                return slot;
            }
        }
    }
    throw std::runtime_error(
        "NLang backend: static field without a slot: " + field.Name());
}

//Task 7: the module slot a static local addresses. The declaration
//statement registered the name before any use can emit (lexical
//order), so a miss is a registration-order bug, not a tolerated one.
//0.13.10: the declarator's stamp answers first — a static local
//declared in the real frame but read inside a nested capture method
//emits in the synthetic frame, whose per-function staticLocalSlots
//map never saw the name.
uint16_t VmBackend::StaticLocalSlotIndexOf(const SnField& field) const {
    auto* pLocal = dynamic_cast<const SnLocalVar*>(&field);
    if (pLocal && pLocal->StaticSlotIdx() != SnLocalVar::kInvalidStaticSlot)
        return pLocal->StaticSlotIdx();
    auto it = m_currFunc->staticLocalSlots.find(field.Name());
    if (it == m_currFunc->staticLocalSlots.end())
        throw std::runtime_error(
            "NLang backend: static local without a slot: " + field.Name());
    return it->second;
}

//0.13.10 Task 6: classify a captured variable's storage. Two shapes:
//the display instance is a plain frame local of the CURRENT frame
//(the declaring paragraph's own frame — the common case), or the
//reference emits inside a nested capture method, whose local 0 is the
//anchor display instance and whose "$outer" fields hop to the owner
//display. A miss in a synthetic frame is an invariant break: the
//declaring paragraph is always on the capturing lambda's paragraph
//chain (lexical scoping), so the hop walk terminates at the owner.
//Hop count from a synthetic frame's anchor display class up the
//"$outer" chain to the OWNING display — by identity of the bound
//display field's parent class, never by field name. A same-named field
//on an intermediate display is a shadowed capture (the declaring
//paragraph's field, not the first name match down the chain, owns the
//storage), and a name walk would stop at the first hop and rebind the
//reference to the shadow. Lexical scoping guarantees termination: the
//declaring paragraph is always on the capturing lambda's paragraph
//chain.
static int OuterHopsToDisplay(SnClassDecl* pStart,
                              SnClassDecl* pDisplayClass)
{
    int hops = 0;
    SnClassDecl* pCls = pStart;
    while (pCls != pDisplayClass) {
        SnField* pOuter = nullptr;
        for (auto& member : pCls->Members())
            if (member.Kind() == NK_ClassField
                && !member.ContainFlags(NF_Static)
                && member.Name() == "$outer")
                pOuter = &member;
        NLANG_ASSERT(pOuter && "$outer chain does not reach the owner display");
        auto* pParent = dynamic_cast<SnClassDecl*>(pOuter->EvalDataType());
        NLANG_ASSERT(pParent);
        ++hops;
        pCls = pParent;
    }
    return hops;
}

VmBackend::BareIdTarget VmBackend::DisplayFieldOfCaptured(SnField* field) {
    NLANG_ASSERT(field);
    SnClassField* pDisplayField = nullptr;
    if (auto* pLocal = dynamic_cast<SnLocalVar*>(field))
        pDisplayField = pLocal->DisplayField();
    else if (auto* pFormal = dynamic_cast<SnFormalParam*>(field))
        pDisplayField = pFormal->DisplayField();
    NLANG_ASSERT(pDisplayField
        && "captured descriptor without a display field");
    auto* pDisplayClass = static_cast<SnClassDecl*>(pDisplayField->Parent());
    NLANG_ASSERT(pDisplayClass && pDisplayClass->ContainFlags(NF_Generated));

    BareIdTarget t;
    t.owner = pDisplayClass;
    const std::string& name = pDisplayField->Name();
    auto it = m_currFunc->localOffsets.find(pDisplayClass->Name());
    if (it != m_currFunc->localOffsets.end()) {
        //The display instance is this frame's local under the class name.
        t.kind = BareIdTarget::DisplayField;
        t.localOffset = it->second;
        t.fieldOff = FindClassFieldOffset(*pDisplayClass, name);
        NLANG_ASSERT(t.fieldOff >= 0);
        return t;
    }
    //Synthetic capture frame: local 0 is the anchor instance; hop the
    //"$outer" fields to the owning display, then read the field there.
    NLANG_ASSERT(m_pCurrClass && m_pCurrClass->ContainFlags(NF_Generated)
        && "captured reference outside any display-bearing frame");
    t.kind = BareIdTarget::CaptureField;
    t.outerHops = OuterHopsToDisplay(m_pCurrClass, pDisplayClass);
    t.fieldOff = FindClassFieldOffset(*pDisplayClass, name);
    NLANG_ASSERT(t.fieldOff >= 0);
    return t;
}

} //namespace nlang
