/*---
    ResolveBareIdentifier.cpp — bare identifier target classification.
    The single source for what a bare name addresses (local / static
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

VmBackend::BareIdTarget VmBackend::ResolveBareIdentifier(SnField* field) {
    BareIdTarget t;
    if (!field)
        return t;
    //v0.12.6 Task 7 (spec §8): a static local addresses its module
    //slot. The check is field-first — the binding node decides, so a
    //same-named frame local (flat frames reuse names) can never
    //shadow the slot-backed one or vice versa. SnLocalVar shares
    //NK_FormalParam as its kind; only locals carry NF_Static on it.
    if (field->Kind() == NK_FormalParam && field->ContainFlags(NF_Static)) {
        t.kind = BareIdTarget::StaticSlot;
        t.slotIdx = StaticLocalSlotIndexOf(*field);
        return t;
    }
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
    if (auto* pOwner = OwningTypeOfMemberField(field)) {
        //0.9.11 D2: struct fields flatten through the struct helper (the
        //class helper walks the inheritance chain, which structs lack).
        int off = (field->Kind() == NK_StructField)
            ? FindFieldOffset(static_cast<SnStructDecl&>(*pOwner),
                              field->Name())
            : FindClassFieldOffset(static_cast<SnClassDecl&>(*pOwner),
                                   field->Name());
        if (off >= 0) {
            t.kind = BareIdTarget::ThisField;
            t.owner = pOwner;
            t.fieldOff = off;
            return t;
        }
    }
    return t;
}

//v0.12.6 Task 8: own fields answer from the registration map; an
//imported stub field (NF_Imported owner, rider valid) instead mints
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
uint16_t VmBackend::StaticLocalSlotIndexOf(const SnField& field) const {
    auto it = m_currFunc->staticLocalSlots.find(field.Name());
    if (it == m_currFunc->staticLocalSlots.end())
        throw std::runtime_error(
            "NLang backend: static local without a slot: " + field.Name());
    return it->second;
}

} //namespace nlang
