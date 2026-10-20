/*---
    Slots.cpp — phase 6 per-unit symbol slot resolution family:
    SlotFor family (own table lookup vs. cross-unit placeholder slot),
    BuildTypeDesc leaf callbacks, constructor slot discovery.
    从 Register.cpp 拆出（phase 6 B1 产码切换战役）。
---*/
#include "VmBackend.h"
#include "builder/SymbolSlots.hpp"
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/SyntaxTree.h>
#include "builder/ModuleRegistry.h"
#include <stdexcept>
#include <string>

namespace nlang {

//Phase 6 cross-unit reference slots (SymbolSlots.hpp supplies the table
//mechanics). All four helpers discriminate by IsOwnUnit first and THROW
//when an own declaration misses its table: a silent placeholder would
//target the unit's own module path and could "link" to itself, hiding
//the registration-order bug. Imported ENUM stubs never enter
//m_enumIndexMap — safe, because only the parser creates SnEnumDecl
//(a .ncu import builds no enum AST node), so every pEnumDecl reaching
//EnumSlotFor is a registered source enum and the own-miss throw
//cannot fire.
uint32_t VmBackend::FunctionSlotFor(SnFunction& callee) {
    if (IsOwnUnit(callee)) {
        auto it = m_funcIndexMap.find(&callee);
        if (it == m_funcIndexMap.end())
            throw std::runtime_error(
                "NLang backend: own function missing from the unit table: "
                + KeyOf(callee));
        return static_cast<uint32_t>(it->second);
    }
    //Cross-unit methods/constructors carry the owner metadata computed
    //below in the slot record. Arity follows the TABLE convention
    //(formals + this for class, struct, and enum instance methods,
    //mirroring AllocParamsAndDefaults): the import record must match the
    //owning unit's registration or nlink's name+paramCount+staticness
    //resolution reports a signature miss. Statics take no this slot
    //(their producer frame has none) and no owner key — staticness alone
    //is the linkage disambiguator.
    const SyntaxNode* pParent = callee.Parent();
    const bool isStatic = callee.ContainFlags(NF_Static);
    const bool needsThis = pParent
        && (pParent->Kind() == NK_ClassDecl
            || pParent->Kind() == NK_StructDecl
            || pParent->Kind() == NK_EnumDecl)
        && !isStatic;
    auto meta = CrossUnitOwnerMeta(callee);
    return FunctionSymbolSlot(m_compiledModule,
        SymbolSlotModulePath(*m_pRegistry, callee), KeyOf(callee),
        static_cast<uint32_t>(
            callee.Params().size() + (needsThis ? 1 : 0)),
        meta.ownerClassKey, isStatic, meta.ownerKind, meta.ctorOrdinal);
}

//v2.6 owner metadata for the cross-unit import record (phase6 design
//section 2: bare table keys cannot disambiguate two same-named
//same-arity members of one unit). A non-static class/struct member
//carries the owner's qualified key and table kind (1 = class, 2 =
//struct — a bare key cannot tell a struct ctor from a same-named class
//method); enum methods and 0.9.10 D5 interface statics keep the
//namespace branch (kind 0): KeyOf gives them the enclosure-qualified
//key and the linker resolves them against the per-unit tables' function
//records. 0.9.11 D2: struct constructors join the class rule (owner key
//= the struct's qualified key, arity carries this). Overloaded
//constructors share name+paramCount (Vec(int) vs Vec(float)), so a
//constructor additionally carries its ordinal in the owner's
//declaration-order registered-ctor list — the producer's
//constructorIndices index. IsRegisteredCtor is the producer's fill
//filter and the stub rebuild replays it in order, so the ordinal
//computed here addresses the right record on the wire. A non-ctor
//member keeps 0xFFFF (the linker's method branch matches by
//name+paramCount+staticness).
VmBackend::CtorOwnerMeta VmBackend::CrossUnitOwnerMeta(
        const SnFunction& callee) const {
    CtorOwnerMeta meta;
    const SyntaxNode* pParent = callee.Parent();
    if (!pParent || callee.ContainFlags(NF_Static)
        || (pParent->Kind() != NK_ClassDecl
            && pParent->Kind() != NK_StructDecl))
        return meta;
    auto& owner = static_cast<const SnField&>(*pParent);
    meta.ownerClassKey = KeyOf(owner);
    meta.ownerKind = (pParent->Kind() == NK_ClassDecl) ? 1 : 2;
    if (callee.Name() != owner.Name())
        return meta;
    auto& members = (pParent->Kind() == NK_ClassDecl)
        ? static_cast<const SnClassDecl&>(*pParent).Members()
        : static_cast<const SnStructDecl&>(*pParent).Members();
    uint16_t ordinal = 0;
    for (auto& member : members) {
        if (member.Kind() != NK_Function || member.Name() != owner.Name())
            continue;
        auto& ctor = static_cast<const SnFunction&>(member);
        //0.9.10 D2.4: a same-name static is not the constructor.
        if (ctor.ContainFlags(NF_Static) || !IsRegisteredCtor(ctor))
            continue;
        if (&ctor == &callee) {
            meta.ctorOrdinal = ordinal;
            break;
        }
        ++ordinal;
    }
    return meta;
}

uint32_t VmBackend::ClassSlotFor(SnClassDecl& decl) {
    if (IsOwnUnit(decl)) {
        int idx = m_compiledModule.FindClass(KeyOf(decl));
        if (idx < 0)
            throw std::runtime_error(
                "NLang backend: own class missing from the unit table: "
                + KeyOf(decl));
        return static_cast<uint32_t>(idx);
    }
    return ClassSymbolSlot(m_compiledModule,
        SymbolSlotModulePath(*m_pRegistry, decl), KeyOf(decl));
}

uint32_t VmBackend::StructSlotFor(SnStructDecl& decl) {
    if (IsOwnUnit(decl)) {
        int idx = m_compiledModule.FindStruct(KeyOf(decl));
        if (idx < 0)
            throw std::runtime_error(
                "NLang backend: own struct missing from the unit table: "
                + KeyOf(decl));
        return static_cast<uint32_t>(idx);
    }
    return StructSymbolSlot(m_compiledModule,
        SymbolSlotModulePath(*m_pRegistry, decl), KeyOf(decl));
}

uint32_t VmBackend::EnumSlotFor(SnEnumDecl& decl) {
    //Own enums: the registration order IS the enum table order
    //(m_enumIndexMap). Cross-unit: named placeholder slot. The own-miss
    //throw mirrors the other slot helpers (see FunctionSlotFor).
    if (IsOwnUnit(decl)) {
        auto it = m_enumIndexMap.find(&decl);
        if (it == m_enumIndexMap.end())
            throw std::runtime_error(
                "NLang backend: own enum missing from the unit table: "
                + KeyOf(decl));
        return static_cast<uint32_t>(it->second);
    }
    return EnumSymbolSlot(m_compiledModule,
        SymbolSlotModulePath(*m_pRegistry, decl), KeyOf(decl));
}

//BuildTypeDesc leaf wiring (TypeDesc.h): named leaves slot through the
//same helpers as every other table reference — the arms are kind-gated
//in TypeDesc.cpp, so a cross-unit field or signature type lands as an
//import placeholder instead of degrading to RTK_NonSerialized.
TypeLeafSlots VmBackend::LeafSlotResolvers() {
    TypeLeafSlots slots;
    slots.structSlotOf = [this](SnField& typeNode) {
        return StructSlotFor(static_cast<SnStructDecl&>(typeNode));
    };
    slots.classSlotOf = [this](SnField& typeNode) {
        return ClassSlotFor(static_cast<SnClassDecl&>(typeNode));
    };
    return slots;
}

uint16_t VmBackend::CtorSlotFor(SnClassDecl& decl, uint16_t classSlot) {
    //0.9.11 T6 (D10) invariant: callers reach this only when the
    //resolver recorded no selected ctor (every user-type call site
    //routes through the AST-selected ctor via FunctionSlotFor) — i.e.
    //builtin/generic types with their single intrinsic ctor, and
    //ctor-less types answering 0xFFFF.
    //Own/builtin record (invariant: own entries occupy 0..n-1): the
    //table carries the ctor indices — PopulateClassMethods writes user
    //ctors in declaration order, the builtin writers push the intrinsic
    //ctor. [0] = that intrinsic ctor.
    const uint32_t ownCount = static_cast<uint32_t>(
        m_compiledModule.classes.size()
        - m_compiledModule.classImports.size());
    if (classSlot < ownCount) {
        const auto& ctors =
            m_compiledModule.classes[classSlot].constructorIndices;
        return ctors.empty() ? 0xFFFF : ctors[0];
    }
    //Cross-unit placeholder: no metadata by design — discover the ctor
    //declaration in the AST (PopulateClassMethods' Name() rule) and
    //slot it through FunctionSlotFor, which carries the v2.6 ownerKind
    //+ ctorOrdinal record. Post-T6 a user-type call always gets a
    //resolver selection (stub classes rebuild their ctor members in
    //constructorIndices order, source-peer classes carry the full AST),
    //so the scan only answers ctor-less placeholders; the shared
    //IsRegisteredCtor filter keeps the answer in step with what
    //codegen could register.
    for (auto& member : decl.Members()) {
        if (member.Kind() != NK_Function
            || member.Name() != decl.Name())
            continue;
        auto& ctor = static_cast<SnFunction&>(member);
        //0.9.10 D2.4: a same-name static is not the constructor.
        if (ctor.ContainFlags(NF_Static))
            continue;
        if (!IsRegisteredCtor(ctor))
            continue;
        return static_cast<uint16_t>(FunctionSlotFor(ctor));
    }
    return 0xFFFF;
}

} //namespace nlang
