/*---
    TypeArgSpelling.cpp — TypeArgSpelling.h契约：规范拼写、AST侧实参
    分类与VM行键（LinkKeyOf）。行键规则随0.12.3字典模型与0.12.6预置
    成员裁定演进，自Register.cpp整块迁入（2026-11-09层界收正：解析器
    与后端共用一份分类，编译器库不得依赖虚拟机库的符号）。
---*/
#include <nlang/compiler/TypeArgSpelling.h>
#include "ModuleRegistry.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/vm/CompiledModuleRecords.h>
#include <string>
#include <vector>

namespace nlang
{

namespace
{
//Array spelling: element spelling + one bracket pair whose inner comma
//count encodes the rank ("int[]", "int[,]"), matching SnArrayTypeToken's
//display form.
std::string ArraySpelling(const std::string& elemSpelling, uint8_t rank)
{
	std::string spelling = elemSpelling;
	spelling += "[";
	for (uint8_t i = 1; i < rank; ++i)
		spelling += ",";
	spelling += "]";
	return spelling;
}
} //namespace

std::string CanonicalArgSpelling(const TypeArgDesc& arg)
{
	if (arg.nested.empty())
		return arg.spelling;
	//Array arguments carry a complete leaf spelling ("int[]", "int[,]");
	//the nested element descriptor is structure for the dispatch paths,
	//never part of the canonical render.
	if (arg.resolvedKind == RTK_Array)
		return arg.spelling;
	std::string spelling = arg.spelling;
	spelling += "<";
	for (size_t i = 0; i < arg.nested.size(); ++i)
	{
		if (i)
			spelling += ",";
		spelling += CanonicalArgSpelling(arg.nested[i]);
	}
	spelling += ">";
	return spelling;
}

//--- VM row keys -------------------------------------------------------

bool IsTypeMember(const SyntaxNode& node) {
    const SyntaxNode* pParent = node.Parent();
    return pParent && (pParent->Kind() == NK_ClassDecl
        || pParent->Kind() == NK_StructDecl
        || pParent->Kind() == NK_InterfaceDecl
        || pParent->Kind() == NK_EnumDecl);
}

namespace
{
std::string QualifiedName(const ModuleRegistry& reg, const SnField& field);

//Enclosure-qualified key of an enum or interface-static method:
//"<unit>.<Type>.<method>" ("lib.Color.rank", "lib.I.twice"; a nested
//type walks out as "lib.Outer.Inner.rank"). Phase 6 §2 D-ruling
//(2026-10-02): enum methods never dispatch by name — direct calls use
//function-table indices — so a bare "rank" cannot disambiguate two
//same-named methods of different enums in one unit. 0.9.10 D5: interface
//statics take the same shape — a bare key would collide with namespace-
//level functions of the same name, and the cross-unit import resolves
//through the ordinary namespace branch by construction.
std::string EnclosureMethodKey(const ModuleRegistry& reg, const SnField& method) {
    std::vector<const SnField*> enclosure;   //owning enum first, then outer types
    const SyntaxNode* p = method.Parent();
    while (p && (p->Kind() == NK_EnumDecl || p->Kind() == NK_ClassDecl
        || p->Kind() == NK_StructDecl || p->Kind() == NK_InterfaceDecl)) {
        enclosure.push_back(static_cast<const SnField*>(p));
        p = p->Parent();
    }
    //The outermost type's parent is a namespace, so its key comes from
    //the registry branch of QualifiedName; inner types append unqualified names.
    std::string key = QualifiedName(reg, *enclosure.back());
    for (auto it = enclosure.rbegin() + 1; it != enclosure.rend(); ++it) {
        key += '.';
        key += (*it)->Name();
    }
    key += '.';
    key += method.Name();
    return key;
}

//Canonical VM key of a declaration: "<package>.<name>", or the unqualified name
//when the node carries no owner tag. The package comes from the compile-
//time registry (path-derived), NOT from an AST namespace walk — a walk can
//only name a unit that literally wrote `namespace <path>`, and project
//members hang off the root, so every project key would stay bare.
//A CLASS/INSTANCE METHOD always keeps an unqualified name, even when its owning
//type is package-nested: methods dispatch by name through their receiver
//(VmExecutor::FindMethodByName compares the unqualified name), so a package
//prefix would make every lookup fail. The exceptions take their
//enclosure-qualified key instead (EnclosureMethodKey above): enum methods
//and interface statics never dispatch by name through a receiver, so
//nothing loses by the prefix, while same-named members of different types
//(and namespace-level functions) stay distinct keys.
//A synthetic generic instantiation keeps its ERASED builtin key ("List"
//for List<int>): the backing CompiledClass is registered once under the
//base name, and the instantiation node is ownerless, so the registry
//would otherwise degrade to its display name ("List<int>") — not a key.
//Exported as LinkKeyOf (TypeArgSpelling.h). TWO-KEY CONTRACT (0.13.8
//item 8): the row-key space and the descriptor-identity space each have
//ONE producer and the two functions must never cross — this function is
//the sole source of row keys and VM table keys (VmBackend::KeyOf is its
//codegen-side spelling); DescriptorSpellingOf below is the sole source
//of descriptor rows and intern keys. They AGREE on prelude/root-package/
//ownerless members (both bare) and on non-generic package members (both
//qualified); they deliberately DIVERGE on generic templates and
//instantiations, where the row stays erased (one shared row per
//template, v0.12.3 dictionary model) while the descriptor spelling
//carries the qualified link identity (a.Pipe<int> vs b.Pipe<int>).
std::string QualifiedName(const ModuleRegistry& reg, const SnField& field) {
    if (field.Kind() == NK_ClassDecl) {
        auto& cls = static_cast<const SnClassDecl&>(field);
        //v0.12.3 dictionary model: the erasure key is the UNQUALIFIED name for
        //the template row and every instantiation alike — the template
        //registers the one shared CompiledClass and ClassSlotFor looks
        //it up by the unqualified name from both node shapes. A package
        //prefix would split the two spellings (the instantiation keys
        //BaseName, the template would key package-qualified).
        if (cls.IsGenericInstantiation() || cls.IsGenericDeclaration())
            return cls.BaseName();
    }
    if (IsTypeMember(field)) {
        if (field.Parent()->Kind() == NK_EnumDecl)
            return EnclosureMethodKey(reg, field);
        //0.9.10 D5: an interface static takes the qualified key; the
        //abstract instance members keep unqualified names (they register no
        //function record at all).
        if (field.Parent()->Kind() == NK_InterfaceDecl
            && field.ContainFlags(NF_Static))
            return EnclosureMethodKey(reg, field);
        return field.Name();
    }
    //Prelude top-level members key bare (builtin-decls source-of-truth
    //design §6.7): the records land in every image (IsOwnUnit's prelude
    //arm) and must spell exactly what bare user code resolves —
    //"Exception", not "lang.Exception".
    if (reg.IsPreludeModule(reg.OwnerOf(field)))
        return field.Name();
    return reg.QualifiedName(field);
}
} //namespace

std::string LinkKeyOf(const ModuleRegistry& reg, const SnField& field) {
    return QualifiedName(reg, field);
}

namespace
{
//The shared bare-or-qualified arm of the descriptor identity: the
//member's own package dotted with its name. Bare for prelude members
//and ownerless nodes; every OWNED member spells its registry module
//path. That path is what makes the spelling resolvable at runtime: the
//linked module's plain-class keys ARE these module-qualified names
//(KeyOf/LinkKeyOf), so a leaf argument hits its class row exactly; a
//generic template's spelling is qualified but its table row stays the
//v0.12.3 erasure-bare name, and the runtime resolves that shape through
//the unique-suffix bridge (FindClassBareOrUniqueSuffix). Producer and
//consumer images converge on the same module path (the one
//SymbolImport.modulePath carries) — imports, inline library TUs and
//project units alike. Unlike QualifiedName this space has no
//generic-erasure arm — see DescriptorSpellingOf.
std::string PackageQualifiedName(const ModuleRegistry& reg,
    const SnField& field) {
    if (reg.IsPreludeModule(reg.OwnerOf(field)))
        return field.Name();
    const std::string package = reg.PackageOf(field);
    if (package.empty())
        return field.Name();
    return package + '.' + field.Name();
}
} //namespace

std::string DescriptorSpellingOf(const ModuleRegistry& reg,
    const SnField& field) {
    //A declared instantiation's synthetic node carries no owner tag, so
    //the package comes from the TEMPLATE it was created from. A builtin
    //instantiation (List<int>) has no declared template — InstanceOf
    //answers null and the spelling stays bare, matching the erasure key.
    if (field.Kind() == NK_ClassDecl) {
        auto& cls = static_cast<const SnClassDecl&>(field);
        if (cls.IsGenericInstantiation()) {
            const SnClassDecl* pTemplate =
                reg.DeclaredGenerics().InstanceOf(&cls);
            if (pTemplate == nullptr)
                return cls.BaseName();
            return PackageQualifiedName(reg, *pTemplate);
        }
    }
    else if (field.Kind() == NK_InterfaceDecl) {
        auto& iface = static_cast<const SnInterfaceDecl&>(field);
        if (iface.IsGenericInstantiation()) {
            const SnInterfaceDecl* pTemplate =
                reg.DeclaredGenerics().InstanceOfInterface(&iface);
            if (pTemplate == nullptr)
                return iface.BaseName();
            return PackageQualifiedName(reg, *pTemplate);
        }
    }
    return PackageQualifiedName(reg, field);
}

//--- AST-side argument classification -----------------------------------

namespace
{
//Class-typed argument, three shapes: a type-parameter placeholder
//(position-encoded, runtime-composable; the spelling is the parameter
//name — diagnostic surface only, canonical keys never contain it, the
//composition-record path replaces the argument at runtime), a nested instantiation
//(descriptor spelling per the two-key contract, the argument tree
//rides `nested`), or an ordinary class/interface (descriptor spelling
//likewise; rows are
//per-module and remap at link time, so the runtime resolves by name,
//not row).
TypeArgDesc ClassArgOf(const ModuleRegistry& reg,
	const SnClassDecl& cls)
{
	TypeArgDesc desc;
	if (cls.IsTypeParamPlaceholder())
	{
		const size_t ordinal =
			reg.DeclaredGenerics().PlaceholderIndex(&cls);
		desc.spelling = cls.Name();
		desc.resolvedKind = RTK_TypeParam;
		desc.rowRef = kTypeParamArgBase
			- static_cast<int32_t>(ordinal);
		return desc;
	}
	if (cls.IsGenericInstantiation())
	{
		desc.spelling = DescriptorSpellingOf(reg, cls);
		desc.resolvedKind = RTK_Class;
		desc.nested = BuildTypeArgsOf(reg, cls);
		return desc;
	}
	desc.spelling = DescriptorSpellingOf(reg, cls);
	desc.resolvedKind = RTK_Class;
	return desc;
}

//Interface-typed argument (0.13.1c Task 8): a nested instantiation of a
//generic interface walks its argument tree (descriptor spelling per the
//two-key contract); a plain interface spells its descriptor spelling.
//This also
//retires the "?" degrade interface arguments used to hit, whose
//spellings collided in the intern keys of distinct interfaces.
TypeArgDesc InterfaceArgOf(const ModuleRegistry& reg,
	const SnInterfaceDecl& iface)
{
	TypeArgDesc desc;
	if (iface.IsGenericInstantiation())
	{
		desc.spelling = DescriptorSpellingOf(reg, iface);
		desc.resolvedKind = RTK_Class;
		desc.nested = BuildTypeArgsOf(reg, iface);
		return desc;
	}
	desc.spelling = DescriptorSpellingOf(reg, iface);
	desc.resolvedKind = RTK_Class;
	return desc;
}
} //namespace

namespace
{
//Leaf arms of BuildTypeArgOf's switch — string and the void return
//slot are rowless keywords; struct/enum leaves carry their link key
//(enums are int32 at runtime, spec §4; the spelling keeps the enum
//name for typeName).
TypeArgDesc LeafArgOf(const ModuleRegistry& reg, const SnField& arg)
{
	TypeArgDesc desc;
	switch (arg.Kind())
	{
	case NK_String:
		desc.spelling = "string";
		desc.resolvedKind = RTK_String;
		return desc;
	//func<void>'s return slot is an NK_Void node (the grammar's void
	//sister productions). The leaf must round-trip as the keyword so the
	//runtime leaf resolver admits it back — the old fall-through to the
	//"?" degrade detonated at container-creation intern time with
	//"unresolved link key" (0.13.10 lambda review).
	case NK_Void:
		desc.spelling = "void";
		desc.resolvedKind = RTK_Void;
		return desc;
	case NK_StructDecl:
		desc.spelling = LinkKeyOf(reg, arg);
		desc.resolvedKind = RTK_Struct;
		return desc;
	case NK_EnumDecl:
		desc.spelling = LinkKeyOf(reg, arg);
		desc.resolvedKind = RTK_Int32;
		return desc;
	default:
		break;
	}
	return desc;
}
} //namespace

TypeArgDesc BuildTypeArgOf(const ModuleRegistry& reg, const SnField& arg)
{
	TypeArgDesc desc;
	//Scalar family (int/float/byte../bool/char): canonical keyword name
	//and the RTK the dispatch paths read.
	if (int pi = ScalarPrimIndexOf(arg.Kind()); pi >= 0)
	{
		desc.spelling = kScalarPrims[pi].name;
		desc.resolvedKind = kScalarPrims[pi].rtk;
		return desc;
	}
	switch (arg.Kind())
	{
	case NK_String:
	case NK_Void:
	case NK_StructDecl:
	case NK_EnumDecl:
		return LeafArgOf(reg, arg);
	case NK_ArrayTypeToken:
	{
		auto& token = static_cast<const SnArrayTypeToken&>(arg);
		TypeArgDesc elem = BuildTypeArgOf(reg, *token.ElemTypeOf());
		desc.spelling = ArraySpelling(elem.spelling, token.Rank());
		desc.resolvedKind = RTK_Array;
		desc.rank = token.Rank();
		desc.nested.push_back(std::move(elem));
		return desc;
	}
	case NK_ClassDecl:
		return ClassArgOf(reg, static_cast<const SnClassDecl&>(arg));
	case NK_InterfaceDecl:
		return InterfaceArgOf(reg,
			static_cast<const SnInterfaceDecl&>(arg));
	default:
		//Unreachable through resolved instantiations: func arguments
		//arrive as SnClassDecl instantiations through ClassArgOf above
		//(Phase 13), their void return slot through the NK_Void arm.
		//Degrade so intern keys never collide, and loudly — the runtime
		//leaf resolver throws on the "?" this spells.
		desc.spelling = "?";
		desc.resolvedKind = RTK_NonSerialized;
		return desc;
	}
}

std::vector<TypeArgDesc> BuildTypeArgsOf(const ModuleRegistry& reg,
	const SnClassDecl& inst)
{
	std::vector<TypeArgDesc> args;
	args.reserve(inst.GenericTypeArgs().size());
	for (const SnField* pArg : inst.GenericTypeArgs())
		args.push_back(BuildTypeArgOf(reg, *pArg));
	return args;
}

std::vector<TypeArgDesc> BuildTypeArgsOf(const ModuleRegistry& reg,
	const SnInterfaceDecl& inst)
{
	std::vector<TypeArgDesc> args;
	args.reserve(inst.GenericTypeArgs().size());
	for (const SnField* pArg : inst.GenericTypeArgs())
		args.push_back(BuildTypeArgOf(reg, *pArg));
	return args;
}

bool HasTypeParamArg(const std::vector<TypeArgDesc>& args)
{
	for (const auto& arg : args)
	{
		if (IsTypeParamArg(arg))
			return true;
		//A nested instantiation containing a type parameter anywhere in
		//its subtree is composable too.
		if (!arg.nested.empty() && HasTypeParamArg(arg.nested))
			return true;
	}
	return false;
}

} //namespace nlang
