/*---
    Register.cpp — 类型与函数注册族：structs/arrays/enums/functions（类族在 RegisterClass.cpp）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include "builder/SymbolSlots.hpp"
#include <nlang/compiler/SyntaxTree.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/compiler/TranslationUnit.h>
#include <nlang/compiler/Logger.h>
#include <nlang/runtime/NodeConsts.h>
#include "builder/ModuleRegistry.h"
#include "BuildEnvironment.h"
#include <cassert>
#include <vector>

namespace nlang {
namespace {

//v1.9 (debugger): source file path recorded per function. Imported
//stubs carry a location whose TransUnit() is null and are normally
//filtered by the body-less check inside RegisterFunctions; the null
//guards are defensive cover for anything that slips through. Hoisted
//here (phase 5) so the entry-point scan can name candidate files too.
static std::string SourceFilePathOf(const SnFunction& func) {
    auto* pLoc = func.Location();
    if (!pLoc) return std::string();
    auto* pTu = pLoc->TransUnit();
    return pTu ? pTu->FilePath() : std::string();
}

} // namespace

//True when the node sits directly inside a class/interface/enum body —
//i.e. it is a type member (method/field), not a namespace member.
//Shared by QualifiedName (members keep bare keys) and the entry-point
//scan (methods are never the entry).
static bool IsTypeMember(const SyntaxNode& node) {
    const SyntaxNode* pParent = node.Parent();
    return pParent && (pParent->Kind() == NK_ClassDecl
        || pParent->Kind() == NK_InterfaceDecl
        || pParent->Kind() == NK_EnumDecl);
}

static std::string QualifiedName(const ModuleRegistry& reg, const SnField& field);

//Enclosure-qualified key of an ENUM method: "<unit>.<Type>.<method>"
//("lib.Color.rank"; an enum nested in a class walks out as
//"lib.Outer.Inner.rank"). Phase 6 §2 D-ruling (2026-10-02): enum
//methods never dispatch by name — direct calls use function-table
//indices — so a bare "rank" cannot disambiguate two same-named methods
//of different enums in one unit. The qualified key lets cross-unit
//enum-method imports resolve through the ordinary namespace branch,
//by construction rather than bare-name luck.
static std::string EnumMethodKey(const ModuleRegistry& reg, const SnField& method) {
    std::vector<const SnField*> enclosure;   //owning enum first, then outer types
    const SyntaxNode* p = method.Parent();
    while (p && (p->Kind() == NK_EnumDecl || p->Kind() == NK_ClassDecl
        || p->Kind() == NK_StructDecl || p->Kind() == NK_InterfaceDecl)) {
        enclosure.push_back(static_cast<const SnField*>(p));
        p = p->Parent();
    }
    //The outermost type's parent is a namespace, so its key comes from
    //the registry branch of QualifiedName; inner types append bare names.
    std::string key = QualifiedName(reg, *enclosure.back());
    for (auto it = enclosure.rbegin() + 1; it != enclosure.rend(); ++it) {
        key += '.';
        key += (*it)->Name();
    }
    key += '.';
    key += method.Name();
    return key;
}

//Canonical VM key of a declaration: "<package>.<name>", or the bare name
//when the node carries no owner tag. The package comes from the compile-
//time registry (path-derived), NOT from an AST namespace walk — a walk can
//only name a unit that literally wrote `namespace <path>`, and project
//members hang off the root, so every project key would stay bare.
//A CLASS/INTERFACE METHOD always keeps a bare name, even when its
//owning type is package-nested: methods dispatch by name through their
//receiver (VmExecutor::FindMethodByName compares the bare name), so a
//package prefix would make every lookup fail. An ENUM METHOD is the one
//exception — it takes its enclosure-qualified key instead (EnumMethodKey
//above): enum methods never dispatch by name, so nothing loses by the
//prefix, and same-named methods of different enums stay distinct keys.
//A synthetic generic instantiation keeps its ERASED builtin key ("List"
//for List<int>): the backing CompiledClass is registered once under the
//base name, and the instantiation node is ownerless, so the registry
//would otherwise degrade to its display name ("List<int>") — not a key.
//File-local to Register.cpp; VmBackend::KeyOf is the only exported
//spelling.
static std::string QualifiedName(const ModuleRegistry& reg, const SnField& field) {
    if (field.Kind() == NK_ClassDecl) {
        auto& cls = static_cast<const SnClassDecl&>(field);
        if (cls.IsGenericInstantiation())
            return cls.BaseName();
    }
    if (IsTypeMember(field)) {
        if (field.Parent()->Kind() == NK_EnumDecl)
            return EnumMethodKey(reg, field);
        return field.Name();
    }
    return reg.QualifiedName(field);
}

//Codegen-side accessor: every name written into or looked up in the VM
//tables goes through here, so the spelling cannot drift from what the
//resolver reports. Requires the registry injected by ModuleBuilder.
std::string VmBackend::KeyOf(const SnField& field) const {
    assert(m_pRegistry && "VmBackend::KeyOf before SetModuleRegistry");
    return QualifiedName(*m_pRegistry, field);
}

//Phase 6 per-unit codegen: reset the per-unit tables and select the unit
//the registration walks will see.
void VmBackend::BeginUnit(uint32_t unitIdx, const std::string& modulePath) {
    m_currentUnitIdx = unitIdx;
    m_compiledModule = CompiledModule{};
    m_compiledModule.modulePath = modulePath;
    m_compiledModule.name = modulePath;
    m_entryKey.clear();
    m_funcIndexMap.clear();
    m_enumIndexMap.clear();
    m_structFieldTypes.clear();
    m_objectClassIdx = -1;
    m_listClassIdx = -1;
    m_dictClassIdx = -1;
    m_exceptionClassIdx = -1;
    m_nullPtrExcClassIdx = -1;
    m_divZeroExcClassIdx = -1;
    m_oobExcClassIdx = -1;
    m_assertExcClassIdx = -1;
    m_ioExcClassIdx = -1;
    m_defaultEmitting.clear();
}

//Own-unit test for the registration walks and the slot helpers. Stubs
//(NF_Imported) are never own — they resolve to placeholder slots.
bool VmBackend::IsOwnUnit(const SnField& member) const {
    if (member.ContainFlags(NF_Imported))
        return false;
    //Owner-of-CONTEXT (ancestor walk), never the flat owner tag: class
    //and enum members carry no tag of their own, so a foreign unit's
    //method would fall to the NO_OWNER fallback below and ghost-register
    //— fully bytecode-compile — into every unit image, silently bypassing
    //the nlink import path for methods and constructors. The contexts
    //that still resolve NO_OWNER are exactly the built-ins and synthetic
    //generic instantiations: own in every unit by design.
    const uint32_t owner = m_pRegistry->OwnerOfContext(member);
    return owner == m_currentUnitIdx || owner == ModuleRegistry::NO_OWNER;
}

//Phase 6 D5, per declaration: the host DLL is the first-dot segment
//(nlang_<seg>.dll), so a native in a multi-segment package cannot name
//one yet (phase 6 lifts the limit). Returns true (after logging) when
//the declaration must be refused.
bool VmBackend::RejectMultiSegmentNativePackage(SnFunction& func) {
    const std::string pkg = m_pRegistry->PackageOf(func);
    if (pkg.find('.') == std::string::npos)
        return false;
    m_pEnv->Log(CLL_Error,
        "native function '%s': multi-segment package '%s' cannot name a "
        "host DLL yet.", KeyOf(func).c_str(), pkg.c_str());
    return true;
}

//Phase 6 per-unit driver pre-check (VmBackend.h contract): the refusal
//above for every function in the tree. The whole-tree walk — not
//per-function codegen — is the call site because library TUs are
//excluded from codegen: an inlined library declaring a dotted native
//must be refused at the consumer's compile time, not surface as a
//load-time failure nobody diagnostically owns.
bool VmBackend::RejectMultiSegmentNatives(SnNamespace& root) {
    bool refused = false;
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() != NK_Function || !node.ContainFlags(NF_Native))
            return;
        if (RejectMultiSegmentNativePackage(static_cast<SnFunction&>(node)))
            refused = true;
    });
    return refused;
}

void VmBackend::RegisterStructDecl(SnStructDecl& sn) {
    CompiledStruct cs;
    //Same-package duplicates are stopped by the compiler's
    //DuplicateFieldChecker before codegen runs, so the type tables keep
    //no second gate here; a cross-module same key IS the same type (the
    //load-time linker dedups peer images by qualified name).
    cs.name = KeyOf(sn);
    cs.fieldCount = static_cast<uint16_t>(sn.FieldCount());
    std::vector<SnField*> fieldTypes;
    for (auto& field : sn.Members()) {
        cs.fieldNames.push_back(field.Name());
        auto* fieldType = field.EvalDataType();
        //0.7.3 B: array fields bind EvalDataType to their interned
        //array token, so RuntimeTypeKind alone files them as
        //RTK_Array. (Pre-token, EvalDataType degraded to the element
        //type and mis-filed `int[]` as RTK_Int32 in writeStruct.)
        uint16_t ftk = RuntimeTypeKind(fieldType);
        cs.fieldTypeKinds.push_back(ftk);
        cs.fieldStructIndices.push_back(0xFFFF);
        cs.fieldClassIndices.push_back(0xFFFF);
        fieldTypes.push_back(fieldType);
    }
    m_compiledModule.structs.push_back(std::move(cs));
    m_structFieldTypes.push_back(std::move(fieldTypes));
}

void VmBackend::RegisterStructs(SnNamespace& root) {
    ForEachDeclNode(root, [&](SnField& node) {
        //Per-unit: foreign units' structs belong to their own images
        //(IsOwnUnit; stubs were already skipped by IsImported).
        if (node.Kind() == NK_StructDecl && !node.IsImported()
            && IsOwnUnit(node))
            RegisterStructDecl(static_cast<SnStructDecl&>(node));
    });
    //Resolve fieldStructIndices now that all structs are registered.
    //fieldClassIndices are resolved later by ResolveStructClassRefs
    //(after RegisterClasses, since classes are not yet registered here).
    //The node lists are parallel to the OWN struct entries only, so the
    //loop is bounded by them: slot resolution below appends placeholder
    //entries for cross-unit field types, growing structs past the list.
    //Element access is by fresh indexing per statement — StructSlotFor
    //can reallocate m_compiledModule.structs, and a held element
    //reference would dangle across it.
    for (size_t si = 0; si < m_structFieldTypes.size(); ++si) {
        auto& fieldTypes = m_structFieldTypes[si];
        for (size_t i = 0; i < fieldTypes.size(); ++i) {
            auto* ft = fieldTypes[i];
            if (m_compiledModule.structs[si].fieldTypeKinds[i]
                    == RTK_Struct
                && ft && ft->Kind() == NK_StructDecl) {
                uint16_t slot = static_cast<uint16_t>(
                    StructSlotFor(static_cast<SnStructDecl&>(*ft)));
                m_compiledModule.structs[si].fieldStructIndices[i] = slot;
            }
        }
    }
}


void VmBackend::ResolveStructClassRefs() {
    //Own-struct entries only (the node lists are parallel to them);
    //placeholder entries have no field data of their own. Fresh per-
    //statement indexing for the same reallocation reason as
    //RegisterStructs (ClassSlotFor appends to classes, and a struct
    //leaf inside BuildTypeDesc appends to structs).
    for (size_t si = 0; si < m_structFieldTypes.size(); ++si) {
        auto& fieldTypes = m_structFieldTypes[si];
        for (size_t i = 0; i < fieldTypes.size(); ++i) {
            auto* ft = fieldTypes[i];
            if (m_compiledModule.structs[si].fieldTypeKinds[i]
                    == RTK_Class
                && ft && ft->Kind() == NK_ClassDecl) {
                uint16_t slot = static_cast<uint16_t>(
                    ClassSlotFor(static_cast<SnClassDecl&>(*ft)));
                m_compiledModule.structs[si].fieldClassIndices[i] = slot;
            }
        }
        //v1.12: field type descriptors — built here because both the
        //struct and class tables are complete (BuildTypeDesc slots
        //leaves through them). The desc is computed before the
        //push_back statement: a struct leaf slots through
        // LeafSlotResolvers, and the member call's receiver must not be
        //a stale reference from before that append.
        for (size_t i = 0; i < fieldTypes.size(); ++i) {
            TypeDesc td = BuildTypeDesc(fieldTypes[i], LeafSlotResolvers());
            m_compiledModule.structs[si].fieldTypeDescs.push_back(td);
        }
    }
}

//数组类型发现与注册族（RegisterArrayType/WalkArrayType*）位于
//RegisterArrayTypes.cpp（2026-10-02 尺寸守卫触发的拆分）。

//Phase 8e-9b: collect every enum declaration's value-name table into
//m_compiledModule.enumNames, and remember each SnEnumDecl*'s defIdx for
//later codegen (SnCastExpr src-type lookup). Enum declarations may live
//at namespace top level or nested inside class/struct scopes (handled by
//CanBeFuncParentEx, same pattern as RegisterStructs/RegisterClasses).
void VmBackend::RegisterEnums(SnNamespace& root) {
    m_enumIndexMap.clear();
    m_compiledModule.enumNames.clear();
    m_compiledModule.enumKeys.clear();
    //Placeholder slots are appended AFTER registration (codegen), so
    //clearing the import table here can never wipe a live placeholder —
    //it only keeps a premature one from leaving an orphan import entry.
    m_compiledModule.enumImports.clear();
    auto registerEnum = [&](SnEnumDecl& sn) {
        auto defIdx = m_compiledModule.enumNames.size();
        std::vector<std::string> names;
        for (auto& member : sn.Members()) {
            if (member.Kind() == NK_EnumMember) {
                auto& em = static_cast<SnEnumMember&>(member);
                //Enum values are sequential starting at 0; if user provides
                //explicit values that skip numbers, the corresponding slots
                //are filled with empty strings (OP_Enum_to_str will throw
                //"enum value out of range" at runtime if hit). NLang grammar
                //currently only supports implicit sequential values.
                while (names.size() <= static_cast<size_t>(em.Value()))
                    names.push_back(std::string());
                names[static_cast<size_t>(em.Value())] = em.Name();
            }
        }
        m_compiledModule.enumNames.push_back(std::move(names));
        //v2.0: parallel qualified key (nlink name-addresses enum slots).
        m_compiledModule.enumKeys.push_back(KeyOf(sn));
        m_enumIndexMap[&sn] = defIdx;
    };
    ForEachDeclNode(root, [&](SnField& node) {
        //Per-unit: foreign units' enums belong to their own images.
        if (node.Kind() == NK_EnumDecl && !node.IsImported()
            && IsOwnUnit(node))
            registerEnum(static_cast<SnEnumDecl&>(node));
    });
}

//v1.9 (debugger): the source-file spelling lives in SourceFilePathOf
//(anonymous namespace, top of file) — shared with the entry-point scan.

void VmBackend::RegisterFunctions(SnNamespace& root) {
    m_funcIndexMap.clear();
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() != NK_Function)
            return;
        auto& func = static_cast<SnFunction&>(node);
        //Per-unit: only this unit's functions register — cross-unit
        //calls resolve through FunctionSlotFor placeholders. The
        //body/native gate below already skips imported stubs.
        if (!IsOwnUnit(func))
            return;
        //Phase 9f: native declarations register like normal functions
        //(the record carries isNative + param signature); a body-less
        //non-native declaration gets no record.
        if (!func.Body() && !func.ContainFlags(NF_Native))
            return;
        CompiledFunction cf;
        cf.name = KeyOf(func);
        cf.sourceFile = SourceFilePathOf(func);
        m_compiledModule.functions.push_back(std::move(cf));
        m_funcIndexMap[&func] = m_compiledModule.functions.size() - 1;
    });
}
//Entry-candidate collection shared by the per-unit scan and the unit-set
//ambiguity pre-check: every root-level (non-method) `main` declared by a
//PROJECT unit, regardless of which unit owns it. Libraries never provide
//the entry; an untagged node has no unit to be a project member of.
std::vector<SnFunction*> VmBackend::CollectEntryCandidates(
    SnNamespace& root) {
    std::vector<SnFunction*> candidates;
    for (auto& member : root.Members()) {
        if (member.Kind() != NK_Function)
            continue;
        auto& func = static_cast<SnFunction&>(member);
        if (func.Name() != "main")
            continue;
        //A method named main is never the entry: methods dispatch
        //through their receiver (IsTypeMember).
        if (IsTypeMember(func))
            continue;
        const uint32_t owner = m_pRegistry->OwnerOf(func);
        if (owner == ModuleRegistry::NO_OWNER
            || m_pRegistry->IsLibraryModule(owner))
            continue;
        candidates.push_back(&func);
    }
    return candidates;
}

//Shared reporter for the two ambiguity gates (the unit-set pre-check
//RejectAmbiguousUnitEntries and the invariant tripwire in
//FindEntryCandidate): every candidate is named by key and source file.
void VmBackend::LogAmbiguousEntries(
    const std::vector<SnFunction*>& candidates) {
    std::string msg = "entry point is ambiguous:";
    for (auto* pFunc : candidates) {
        msg += " '" + KeyOf(*pFunc) + "' ("
            + SourceFilePathOf(*pFunc) + ");";
    }
    msg += " keep exactly one main() in the project.";
    m_pEnv->Log(CLL_Error, "%s", msg.c_str());
}

//Entry = a root-level (non-method) `main` declared by a PROJECT unit.
//Libraries never provide the entry; a module with zero candidates keeps
//entryPoint = -1 and nvm reports "no entry point" instead of today's
//"no 'main' function found". Same-directory duplicates are rejected by
//the merged-namespace front end at resolve time, and different-directory
//mains by the RejectAmbiguousUnitEntries pre-check, so the >1 branch
//below is an invariant tripwire.
SnFunction* VmBackend::FindEntryCandidate(SnNamespace& root) {
    std::vector<SnFunction*> own;
    for (SnFunction* pFunc : CollectEntryCandidates(root)) {
        //Per-unit: each unit's image resolves only its own entry
        //candidate, so the entry lands in its owning unit's image and
        //every other unit's image stays entry-less.
        const uint32_t owner = m_pRegistry->OwnerOf(*pFunc);
        if (owner != m_currentUnitIdx)
            continue;
        own.push_back(pFunc);
    }
    if (own.empty())
        return nullptr;
    if (own.size() > 1) {
        LogAmbiguousEntries(own);
        return nullptr;
    }
    return own.front();
}

//Phase 6 per-unit driver pre-check (VmBackend.h contract): the per-unit
//owner gate in FindEntryCandidate cannot see sibling units' candidates,
//so the whole unit set is judged here before any image is built.
bool VmBackend::RejectAmbiguousUnitEntries(SnNamespace& root) {
    std::vector<SnFunction*> candidates = CollectEntryCandidates(root);
    if (candidates.size() < 2)
        return false;
    LogAmbiguousEntries(candidates);
    return true;
}

void VmBackend::ResolveEntryPoint(SnNamespace& root) {
    m_compiledModule.entryPoint = -1;
    m_entryKey.clear();
    SnFunction* pEntry = FindEntryCandidate(root);
    if (!pEntry)
        return;   //entryPoint stays -1: no entry point in this module
    if (pEntry->ContainFlags(NF_Native) || !pEntry->Body()) {
        //The native record exists, but executing the entry must not
        //silently dispatch through the host table; a body-less non-native
        //main registers no function record, so its key would dangle —
        //either way the entry is not executable. Name the key.
        m_pEnv->Log(CLL_Error,
            "entry point '%s' is not a compiled function: main must have"
            " a body and not be a host-table declaration.",
            KeyOf(*pEntry).c_str());
        return;
    }
    m_entryKey = KeyOf(*pEntry);
    m_compiledModule.entryPoint = m_compiledModule.FindFunction(m_entryKey);
}

} //namespace nlang
