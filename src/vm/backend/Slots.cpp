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
    //Cross-unit methods/constructors carry the owning class's qualified
    //key in the slot record: bare table keys cannot disambiguate two
    //same-named same-arity methods of one unit (phase6 design section 2).
    //Enum methods DO reach here — KeyOf gives them the enclosure-
    //qualified EnumMethodKey and they resolve through the namespace
    //branch; interface members never do (no function record of their
    //own). Arity follows the TABLE convention (formals + this for class
    //and enum methods, mirroring AllocParamsAndDefaults): the import
    //record must match the owning unit's registration or nlink's
    //name+paramCount resolution reports a signature miss.
    std::string ownerClassKey;
    const SyntaxNode* pParent = callee.Parent();
    const bool isMethod = pParent
        && (pParent->Kind() == NK_ClassDecl
            || pParent->Kind() == NK_EnumDecl);
    if (pParent && pParent->Kind() == NK_ClassDecl)
        ownerClassKey = KeyOf(static_cast<const SnClassDecl&>(*pParent));
    return FunctionSymbolSlot(m_compiledModule,
        SymbolSlotModulePath(*m_pRegistry, callee), KeyOf(callee),
        static_cast<uint32_t>(
            callee.Params().size() + (isMethod ? 1 : 0)),
        ownerClassKey);
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
    //Own/builtin record (invariant: own entries occupy 0..n-1): the
    //table carries the ctor index — PopulateClassMethods writes user
    //ctors, the builtin writers push intrinsic ctors.
    const uint32_t ownCount = static_cast<uint32_t>(
        m_compiledModule.classes.size()
        - m_compiledModule.classImports.size());
    if (classSlot < ownCount)
        return m_compiledModule.classes[classSlot].constructorIdx;
    //Cross-unit placeholder: no metadata by design — discover the ctor
    //declaration in the AST (PopulateClassMethods' Name() rule) and
    //slot it through FunctionSlotFor; registration parity requires a
    //body or native flag. A .ncu STUB class carries no members at all,
    //so the scan below would silently return 0xFFFF and drop the ctor
    //call + arguments; refuse loudly instead. B1b wires stub ctor
    //slots with the import record's arity (NcuLinkerSlots ctor arm).
    if (decl.IsImported())
        throw std::runtime_error(
            "NLang backend: constructing imported class '" + KeyOf(decl)
            + "' from a .ncu stub is not wired in per-unit mode yet");
    for (auto& member : decl.Members()) {
        if (member.Kind() != NK_Function
            || member.Name() != decl.Name())
            continue;
        auto& ctor = static_cast<SnFunction&>(member);
        if (!ctor.Body() && !ctor.ContainFlags(NF_Native))
            continue;
        return static_cast<uint16_t>(FunctionSlotFor(ctor));
    }
    return 0xFFFF;
}

} //namespace nlang
