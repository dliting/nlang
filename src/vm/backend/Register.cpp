/*---
    Register.cpp — 函数注册族与入口点解析（类族在 RegisterClass.cpp、
    struct 族在 RegisterStructs.cpp、接口在 RegisterInterfaces.cpp、
    数组在 RegisterArrayTypes.cpp、枚举在 RegisterEnums.cpp）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include "builder/SymbolSlots.hpp"
#include <string>
#include <nlang/compiler/SyntaxTree.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/compiler/TranslationUnit.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/runtime/NodeConsts.h>
#include "builder/ModuleRegistry.h"
#include "builder/ThreadNamespace.h"
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

//True when the node sits directly inside a class/struct/interface/enum
//body — i.e. it is a type member (method/field/constructor), not a
//namespace member. Shared by QualifiedName (members keep bare keys) and
//the entry-point scan (methods are never the entry). 0.9.11 D2: struct
//constructors join — their table key stays bare like every other type
//member, and a struct-typed `main` is never the entry point.
static bool IsTypeMember(const SyntaxNode& node) {
    const SyntaxNode* pParent = node.Parent();
    return pParent && (pParent->Kind() == NK_ClassDecl
        || pParent->Kind() == NK_StructDecl
        || pParent->Kind() == NK_InterfaceDecl
        || pParent->Kind() == NK_EnumDecl);
}

static std::string QualifiedName(const ModuleRegistry& reg, const SnField& field);

//Enclosure-qualified key of an enum or interface-static method:
//"<unit>.<Type>.<method>" ("lib.Color.rank", "lib.I.twice"; a nested
//type walks out as "lib.Outer.Inner.rank"). Phase 6 §2 D-ruling
//(2026-10-02): enum methods never dispatch by name — direct calls use
//function-table indices — so a bare "rank" cannot disambiguate two
//same-named methods of different enums in one unit. 0.9.10 D5: interface
//statics take the same shape — a bare key would collide with namespace-
//level functions of the same name, and the cross-unit import resolves
//through the ordinary namespace branch by construction.
static std::string EnclosureMethodKey(const ModuleRegistry& reg, const SnField& method) {
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
//A CLASS/INSTANCE METHOD always keeps a bare name, even when its owning
//type is package-nested: methods dispatch by name through their receiver
//(VmExecutor::FindMethodByName compares the bare name), so a package
//prefix would make every lookup fail. The exceptions take their
//enclosure-qualified key instead (EnclosureMethodKey above): enum methods
//and interface statics never dispatch by name through a receiver, so
//nothing loses by the prefix, while same-named members of different types
//(and namespace-level functions) stay distinct keys.
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
            return EnclosureMethodKey(reg, field);
        //0.9.10 D5: an interface static takes the qualified key; the
        //abstract instance members keep bare names (they register no
        //function record at all).
        if (field.Parent()->Kind() == NK_InterfaceDecl
            && field.ContainFlags(NF_Static))
            return EnclosureMethodKey(reg, field);
        return field.Name();
    }
    //Prelude top-level members key bare (builtin-decls source-of-truth
    //design §6.7): the records land in every image (IsOwnUnit's prelude
    //arm) and must spell exactly what bare user code resolves —
    //"Exception", not "builtins.Exception".
    if (reg.IsPreludeModule(reg.OwnerOf(field)))
        return field.Name();
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
    m_listClassIdx = -1;
    m_dictClassIdx = -1;
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
    //Prelude members are own in EVERY unit image too (builtin-decls
    //source-of-truth design §6.7): the prelude unit is a library TU and
    //never generates, so its declared records ride each project image —
    //the declared mirror of the ownerless-builtin semantics above.
    return owner == m_currentUnitIdx || owner == ModuleRegistry::NO_OWNER
        || m_pRegistry->IsPreludeModule(owner);
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


//结构体登记族（RegisterStructDecl/RegisterStructs/ResolveStructClassRefs）
//位于 RegisterStructs.cpp（尺寸守卫触发的拆分，一族一 TU）。

//数组类型发现与注册族（RegisterArrayType/WalkArrayType*）位于
//RegisterArrayTypes.cpp（2026-10-02 尺寸守卫触发的拆分）。

//枚举登记族（RegisterEnums/RegisterEnumMethods）位于
//RegisterEnums.cpp（0.10.14 v2.9 尺寸守卫触发的拆分，一表一 TU）。

//v1.9 (debugger): the source-file spelling lives in SourceFilePathOf
//(anonymous namespace, top of file) — shared with the entry-point scan.

//True when func is a member of a generic class declaration (the
//signature-surface template, not an instantiation). Under erasure naming
//the template member and the instance member share one key, so a template
//record would both collide with the instance's and drag its placeholder-
//typed signature into signature-descriptor emission (whose leaf resolvers
//expect registered classes) — only instantiations' member copies register.
static bool IsGenericTemplateMember(const SnFunction& func) {
    const SyntaxNode* pParent = func.Parent();
    return pParent && pParent->Kind() == NK_ClassDecl
        && static_cast<const SnClassDecl*>(pParent)->IsGenericDeclaration();
}

void VmBackend::RegisterFunctions(SnNamespace& root) {
    m_funcIndexMap.clear();
    //Two-phase fill: the unit's own declarations first, then the
    //prelude/builtin-owned ones. The function-table layout is a
    //cross-process contract — a library build's npkg table and a
    //consumer's shadow replay must interleave identically, but tree
    //insertion order does not survive the trip (a library build parses
    //the library unit before the prelude; a consumer inlines the
    //prelude first, so a single tree-order pass drifts every prelude
    //ordinal and nlink rejects the import record at load). Ownership
    //phase is the invariant both processes agree on.
    for (const bool preludePhase : { false, true }) {
        ForEachDeclNode(root, [&](SnField& node) {
            if (node.Kind() != NK_Function)
                return;
            auto& func = static_cast<SnFunction&>(node);
            //Per-unit: only this unit's functions register — cross-unit
            //calls resolve through FunctionSlotFor placeholders. The
            //body/native gate below already skips imported stubs. A prelude
            //declaration whose bare name a foreign declaration claims does
            //not register (spec §6.8 — the foreign declaration is the
            //visible winner; same rule as the class sites).
            if (!IsOwnUnit(func) || IsShadowedPreludeDecl(func))
                return;
            const uint32_t owner = m_pRegistry->OwnerOfContext(func);
            if ((owner != m_currentUnitIdx) != preludePhase)
                return;
            if (IsGenericTemplateMember(func))
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
    RegisterDeclaredGenericInstanceMembers();
}

//Declared-generic instantiation members: the signature copies register
//like any native member so the method tables and direct call sites can
//index them. Each instantiation mints once, so the records are unique
//per unit image.
void VmBackend::RegisterDeclaredGenericInstanceMembers() {
    m_pRegistry->ForEachOwnGenericInstance(m_currentUnitIdx,
        [&](SnClassDecl& inst) {
        for (auto& member : inst.Members())
        {
            if (member.Kind() != NK_Function)
                continue;
            auto& func = static_cast<SnFunction&>(member);
            if (!func.Body() && !func.ContainFlags(NF_Native))
                continue;
            CompiledFunction cf;
            cf.name = KeyOf(func);
            cf.sourceFile = SourceFilePathOf(func);
            m_compiledModule.functions.push_back(std::move(cf));
            m_funcIndexMap[&func] = m_compiledModule.functions.size() - 1;
        }
    });
}

//The per-unit registration sequence — table layout only. Shared by the
//GenerateUnits loop (before bytecode emission) and the library shadow
//registration below; the order IS the layout contract.
void VmBackend::RegisterUnitTables(SnNamespace& root) {
    RegisterBuiltinClasses();
    RegisterStructs(root);
    RegisterClasses(root);
    ResolveStructClassRefs();
    RegisterArrayTypes(root);
    RegisterEnums(root);
    RegisterFunctions(root);
    PopulateClassMethods(root);
    PopulateStructMethods(root);
    RegisterInterfaces(root);
    RegisterEnumMethods(root);
}

//Same-build cross-unit callees need their producer-side table index for
//the import record (nlink's authoritative selector) — inlined library
//sources AND sibling user units in a project build alike (same-name
//same-arity type overloads link ambiguously without it). The layout is
//built by this backend's own registration sequence, so the index is
//only knowable by RUNNING that sequence — a hand-replayed count would
//drift with every builtin change. Shadow-register every unit once
//(registration only, no bytecode, no image) and archive its function
//map; the layout depends only on the unit's own declarations plus the
//constant builtin set, so it matches the producer exactly.
void VmBackend::ShadowRegisterUnits(SnNamespace& root) {
    const uint32_t count = m_pRegistry->ModuleCount();
    m_unitFuncIdxByModule.assign(count, {});
    for (uint32_t i = 0; i < count; ++i) {
        BeginUnit(i, m_pRegistry->ModulePathOf(i));
        RegisterUnitTables(root);
        auto& archived = m_unitFuncIdxByModule[i];
        archived.reserve(m_funcIndexMap.size());
        for (auto& kv : m_funcIndexMap)
            archived[kv.first] = static_cast<uint16_t>(kv.second);
    }
    //The shadow modules' tables must not leak into a real unit's image:
    //the first real BeginUnit resets every per-unit member anyway, but
    //an empty unit set would leave them standing — clear defensively.
    m_compiledModule = CompiledModule{};
}

uint16_t VmBackend::ProducerIdxOfCrossUnitFunc(const SnFunction& callee) {
    const uint32_t owner = m_pRegistry->OwnerOfContext(callee);
    if (owner == ModuleRegistry::NO_OWNER
        || owner >= m_unitFuncIdxByModule.size())
        return 0xFFFF;
    const auto& map = m_unitFuncIdxByModule[owner];
    const auto it = map.find(const_cast<SnFunction*>(&callee));
    return it != map.end() ? it->second : 0xFFFF;
}

//K1's per-module intrinsic stub append (RegisterThreadNamespaceFunctions)
//is gone: K3 materialized the thread package as stdlib/thread.n native
//declarations, which register through RegisterFunctions like every other
//native and get stamped onto the intrinsic channel by GenerateFunction's
//package-identity branch (GenerateUnits.cpp).

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
