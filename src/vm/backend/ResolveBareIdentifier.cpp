/*---
    ResolveBareIdentifier.cpp — bare identifier target classification.
    The single source for what a bare name addresses (local / static
    slot / static local slot / implicit this-field), consumed by the
    identifier read, plain assignment and compound assignment emitters.
    Split out of VmBackend.cpp (2026-11-02 source-size guard).
---*/
#include "VmBackend.h"
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
            static_cast<const SnClassField&>(*field));
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

uint16_t VmBackend::StaticSlotIndexOf(const SnClassField& field) const {
    auto it = m_staticSlotByField.find(const_cast<SnClassField*>(&field));
    if (it == m_staticSlotByField.end())
        throw std::runtime_error(
            "NLang backend: static field without a slot: " + field.Name());
    return it->second;
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
