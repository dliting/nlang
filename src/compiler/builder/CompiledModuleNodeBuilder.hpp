#pragma once
#include "CompiledModuleStubSupport.hpp"
#include "SyntaxTree.h"
#include "SnMisc.h"
#include "SnData.h"
#include "SnExpressions.h"
#include "SnStatements.h"
#include "BuildEnvironment.h"
#include "nlang/vm/CompiledModule.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/RnTypes.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

namespace nlang
{

//Builds Sn* AST stubs directly from a CompiledModule (the in-memory form of
//a .ncu file), bypassing the legacy RnFunction / RuntimeNode pipeline.
//
//Stubs are intentionally minimal: they carry enough structural information
//(name, param count, member names) for VmBackend to skip codegen on them
//(via NF_Imported + missing Body) and for the resolver to recognize the
//symbol as an imported declaration. Formal and return types come from the
//v1.12 type descriptors: this builder synthesizes plain syntactic type
//expressions (the same shapes the parser emits for hand-written source),
//so the consumer's normal ResolveDataTypes pass binds and interns them
//through the standard channels — cross-module call-site type checking is
//real, not skipped.
//
//After BuildFromCompiledModule, the caller can query ImportedFunctions()
//for the minted stubs (registry owner tagging in ModuleBuilder::
//RegisterExternalStubs). Phase 5: table keys are package-qualified
//("<pkg>.<name>"), but an imported stub's AST NAME must be the leaf —
//the resolver looks stubs up by the caller's bare callee/type name,
//while the stub's owner tag (the external module) supplies the package.
//The qualified key stays in the CompiledModule tables, where the
//load-time linker resolves it from the peer images.
class CompiledModuleNodeBuilder
{
public:
	//Construct a builder for one source module.
	CompiledModuleNodeBuilder(SyntaxTree &tree,
		const std::string &moduleName) :
		m_Tree(tree), m_moduleName(moduleName)
	{
	}

	//Walk cm.functions/classes/structs and append Sn* stubs to m_Tree.Root().
	//Throws std::runtime_error on policy violations (e.g. imported module
	//defining main() — see Layer 6).
	//
	//R10-1 dedup: every compiled .ncu contains built-in functions/classes
	//emitted by RegisterBuiltinClasses (ByteStream/Dict/List constructors,
	//their methods, etc.) because the serializer writes everything in
	//m_compiledModule. The consumer's AST root already has these same
	//built-ins injected by SyntaxTree::BuildFromRuntime before this method
	//runs. Adding root entries for them would trigger DuplicateFieldChecker
	//errors. A function whose name already exists in root stays as a
	//DETACHED stub (registered, never pushed into root — see
	//TakeDetachedStubs()); structs/classes are skipped entirely.
	void BuildFromCompiledModule(const CompiledModule &cm)
	{
		CheckNoMainFunction(cm);

		//Own entries only: a unit image's tables carry placeholder tail
		//slots after the own entries (one per import record, in order).
		//Placeholders target OTHER modules — stubbing them into this
		//consumer's tree would mis-attribute their ownership.
		const size_t ownFuncs =
			cm.functions.size() - cm.functionImports.size();
		const size_t ownStructs =
			cm.structs.size() - cm.structImports.size();
		const size_t ownClasses =
			cm.classes.size() - cm.classImports.size();

		auto loc = CompiledModuleNodeLocation(cm.name);

		//Helper: is there already a top-level field with this name in root?
		//Built-ins are injected by BuildFromRuntime before we run.
		auto nameExistsInRoot = [this](const std::string &name) -> bool {
			for (auto &m : TheRoot().Members())
				if (m.Name() == name)
					return true;
			return false;
		};

		//Build a set of CompiledFunction indices that are actually class
		//methods or constructors (referenced by some CompiledClass's
		//methodIndices / constructorIdx). These must NOT be promoted to
		//free-function stubs — they belong to their owning class and are
		//already represented (via class merge) on the consumer side.
		//Without this filter, List.add / Dict.set / etc. would be promoted
		//to root as free functions, and the user's own free `add` would
		//then be wrongly deduped against the imported List.add (R10-1
		//collision).
		std::unordered_set<uint32_t> methodOrCtorIndices;
		for (size_t ci = 0; ci < ownClasses; ++ci)
		{
			for (uint16_t mi : cm.classes[ci].methodIndices)
				methodOrCtorIndices.insert(mi);
			if (cm.classes[ci].constructorIdx != 0xFFFF)
				methodOrCtorIndices.insert(cm.classes[ci].constructorIdx);
		}

		//Functions first so that subsequent class/struct stub members can
		//reference them by name if needed (resolver does name-based lookup
		//later, so physical order among siblings doesn't matter — but having
		//functions first matches the typical parser emission order).
		for (uint32_t i = 0; i < ownFuncs; ++i)
		{
			if (methodOrCtorIndices.count(i))
				continue;  //class method/ctor — owned by its class, not free
			SnFunction *stub = CreateFunctionStub(cm.functions[i], cm, loc);
			if (nameExistsInRoot(LeafNameOfKey(cm.functions[i].name)))
			{
				//R10-1: no second root entry for an existing name — but
				//the stub stays registered (ImportedFunctions) so its
				//module's stub table remains complete: qualified calls
				//resolve through the stub table, never the shared root.
				//Ownership passes to the caller via TakeDetachedStubs().
				m_detachedStubs.emplace_back(stub);
				m_importedFuncs.push_back(stub);
				continue;
			}
			m_Tree.Root()->Members().push_back(stub);
			m_importedFuncs.push_back(stub);
		}

		//Structs.
		for (size_t si = 0; si < ownStructs; ++si)
		{
			if (nameExistsInRoot(LeafNameOfKey(cm.structs[si].name)))
				continue;  //R10-1
			SnStructDecl *stub = CreateStructStub(cm.structs[si], loc);
			m_Tree.Root()->Members().push_back(stub);
			m_importedTypes.push_back(stub);
		}

		//Classes.
		for (size_t ci = 0; ci < ownClasses; ++ci)
		{
			if (nameExistsInRoot(LeafNameOfKey(cm.classes[ci].name)))
				continue;  //R10-1
			SnClassDecl *stub = CreateClassStub(cm.classes[ci], loc);
			m_Tree.Root()->Members().push_back(stub);
			m_importedTypes.push_back(stub);
		}
	}

	//Stubs built for each CompiledFunction entry, in source-module order.
	//Caller (RegisterExternalStubs) tags each one with its module's
	//registry entry. Complete: it also holds the detached stubs (names
	//that already existed in root), which never became root members.
	const std::vector<SnFunction*>& ImportedFunctions() const
	{
		return m_importedFuncs;
	}

	//Take ownership of the detached stubs (names that already existed
	//in root — R10-1 collisions). They are registered in
	//ImportedFunctions() but are NOT root members; the caller keeps
	//them alive so the registry stub table stays valid until the
	//build ends.
	std::vector<std::unique_ptr<SnFunction>> TakeDetachedStubs()
	{
		return std::move(m_detachedStubs);
	}

	//Struct/class stubs minted into root, in source-module order.
	//Caller (RegisterExternalStubs) tags each with its module's
	//registry entry so FindModuleType can bind "<pkg>.<Type>" through
	//the owner-filtered root scan. Unlike ImportedFunctions there are
	//no detached type stubs: a collision means no stub was minted.
	const std::vector<SnField*>& ImportedTypes() const
	{
		return m_importedTypes;
	}

	const std::string& ModuleName() const
	{
		return m_moduleName;
	}

private:
	//v1.12: synthesize a SYNTACTIC type expression from a serialized type
	//descriptor — the same shape the parser would build for hand-written
	//source, so the consumer's normal ResolveDataTypes pass binds and
	//interns it through the standard channels (named types resolve against
	//the root, where this builder placed the imported struct/class stubs;
	//the List/Dict base names go through the built-in generic
	//instantiation path; builtin scalar kinds arrive pre-bound via the
	//NodeKind ctor). NonSerialized (interface/func types, defensive
	//misses) degrades to the int32 placeholder.
	SnFieldExpr *SynthTypeExprFromDesc(const TypeDesc &td,
		const CompiledModule &cm, const ISourceLocation &loc)
	{
		//0.7.5: scalar kinds (legacy RTK_Int32/RTK_Float and the
		//RTK_Byte..RTK_Char family) resolve through the registry — the
		//old switch handled only RTK_Float, silently degrading every
		//other scalar to the int32 placeholder (an imported `long`
		//formal/return truncated its 8-byte values at consumer call
		//sites before this arm existed).
		if (int pi = ScalarPrimIndexOfRtk(td.kind); pi >= 0)
			return new SnIdentifierExpr(kScalarPrims[pi].kind, loc);
		switch (td.kind)
		{
			case RTK_String:
				return new SnIdentifierExpr(NK_String, loc);
			case RTK_Struct:
				if (td.typeIdx < cm.structs.size())
					return NamedTypeExprFromKey(
						cm.structs[td.typeIdx].name, loc);
				break;  //out-of-range index — int32 placeholder below
			case RTK_Class:
				if (td.typeIdx < cm.classes.size())
					return NamedTypeExprFromKey(
						cm.classes[td.typeIdx].name, loc);
				break;
			case RTK_Array:
				if (!td.elems.empty())
					return new SnArrayTypeExpr(
						SynthTypeExprFromDesc(td.elems[0], cm, loc), loc);
				break;
			case RTK_List:
				if (!td.elems.empty())
				{
					auto *pArgs = new std::vector<SnFieldExpr*>();
					pArgs->push_back(SynthTypeExprFromDesc(td.elems[0], cm, loc));
					return new SnGenericTypeExpr(
						new SnIdentifierExpr(
							new std::string(kBuiltinListTypeName), loc),
						pArgs, loc);
				}
				break;
			case RTK_Dict:
				if (td.elems.size() >= 2)
				{
					auto *pArgs = new std::vector<SnFieldExpr*>();
					pArgs->push_back(SynthTypeExprFromDesc(td.elems[0], cm, loc));
					pArgs->push_back(SynthTypeExprFromDesc(td.elems[1], cm, loc));
					return new SnGenericTypeExpr(
						new SnIdentifierExpr(
							new std::string(kBuiltinDictTypeName), loc),
						pArgs, loc);
				}
				break;
			default:
				break;
		}
		return new SnIdentifierExpr(NK_Int32, loc);
	}

	//Option B Step 4: reconstruct an SnLiteralExpr for a formal default from
	//the serialized DefaultValueDesc. Returns nullptr when the formal has no
	//default (tag == RTK_Void). For RTK_String, the producer's stringConstants
	//index is resolved against cm.stringConstants and the content is embedded
	//directly in the SnLiteralExpr — VmBackend's codegen will re-intern it
	//into the consumer's stringConstants at call-site emission.
	//
	//The returned expression is NF_Imported (set by SnLiteralExpr's RTTI
	//ctor) so resolver/codegen treat it as already-resolved.
	SnExpression *SynthDefaultExpr(const DefaultValueDesc &dv,
		const CompiledModule &cm, const std::string &funcName,
		uint16_t paramIdx, const ISourceLocation &loc)
	{
		switch (dv.tag)
		{
			case RTK_Null:
			{
				auto *lit = new SnLiteralExpr(*RnInt32::Instance(), 0, loc);
				lit->AddFlags(NF_NullLiteral);
				return lit;
			}
			case RTK_Int32:
				return new SnLiteralExpr(*RnInt32::Instance(),
					static_cast<int32_t>(dv.intValue), loc);
			case RTK_Long:
				return new SnLiteralExpr(*RnLong::Instance(),
					static_cast<int64>(dv.longValue), loc);
			case RTK_ULong:
				return new SnLiteralExpr(*RnULong::Instance(),
					static_cast<uint64>(dv.longValue), loc);
			case RTK_Double:
				return new SnLiteralExpr(*RnDouble::Instance(),
					dv.doubleValue, loc);
			case RTK_Float:
				return new SnLiteralExpr(*RnFloat::Instance(),
					dv.floatValue, loc);
			case RTK_String:
			{
				if (dv.stringIdx >= cm.stringConstants.size())
					return nullptr;  //malformed — skip default
				const std::string &content = cm.stringConstants[dv.stringIdx];
				return new SnLiteralExpr(*RnString::Instance(),
					new std::string(content), loc);
			}
			case RTK_Unfoldable:
				//Producer's original default was non-constant-foldable (e.g.
				//`b = helper()` or `b = a + 1`). Cross-module callers can't
				//reconstruct it; reject at consumer-side stub construction
				//(LoadImports catches + logs as compile error).
				throw std::runtime_error(
					"imported function '" + funcName + "' has a "
					"non-constant-foldable default for parameter " +
					std::to_string(static_cast<unsigned long>(paramIdx)) +
					"; cross-module defaults must be literal "
					"(int/float/string/null/negative)");
			default:
				return nullptr;  //RTK_Void or unknown — no default
		}
	}

	//Build a SnFunction stub for an imported CompiledFunction.
	//The stub has no Body() — VmBackend.RegisterFunctions and
	//GenerateAllBytecode already skip bodyless functions, and the
	//NF_Imported flag adds an explicit IsImported() gate for safety.
	SnFunction *CreateFunctionStub(const CompiledFunction &cf,
		const CompiledModule &cm, const ISourceLocation &loc)
	{
		//Return type. RTK_Void → no return type (SnFunction::HasReturn() == false).
		//v1.12: the serialized descriptor supplies the true type; the
		//NonSerialized sentinel (Func/interface returns) degrades to the
		//int32 placeholder.
		SnFieldExpr *pRetType = nullptr;
		if (cf.returnTypeKind != RTK_Void)
			pRetType = SynthTypeExprFromDesc(cf.returnTypeDesc, cm, loc);

		//Synthesize paramCount SnFormalParam nodes with names "p0", "p1",
		//... (stub param names are placeholders; named-arg binding is
		//rejected for imported callees anyway). v1.12: each formal's true
		//type and out flag come from the serialized descriptors (free
		//functions carry exactly paramCount of them — methods never reach
		//stub construction, the methodOrCtorIndices filter owns those).
		//Option B: if cf.defaultValues[i] carries a constant-foldable default,
		//attach the reconstructed SnLiteralExpr to the stub formal so the
		//resolver can apply it at consumer call sites.
		//Use raw PtrList<SnFormalParam>* — SnFunction takes UniquePtrList by
		//value, whose inner_collection* constructor assumes ownership and
		//deletes the source list.
		auto *pParams = new PtrList<SnFormalParam>();
		for (uint16_t i = 0; i < cf.paramCount; ++i)
		{
			auto paramName = new std::string("p" + std::to_string(i));
			NodeBits paramFlags = NF_NONE;
			SnFieldExpr *pParamType;
			if (i < cf.paramTypeDescs.size())
			{
				const ParamTypeDesc &ptd = cf.paramTypeDescs[i];
				if (ptd.flags & PTDF_Out)
					paramFlags |= NF_Out;
				pParamType = SynthTypeExprFromDesc(ptd.type, cm, loc);
			}
			else
			{
				pParamType = new SnIdentifierExpr(NK_Int32, loc);
			}
			SnExpression *pDefault = nullptr;
			if (i < cf.defaultValues.size())
				pDefault = SynthDefaultExpr(cf.defaultValues[i], cm,
					cf.name, i, loc);
			auto *pParam = new SnFormalParam(paramFlags, pParamType, paramName,
				pDefault, loc);
			pParams->push_back(pParam);
		}

		//Mint the function name as a heap string (SnFunction takes ownership).
		//Leaf name: see LeafNameOfKey (phase 5 key vs. stub-name split).
		auto pName = new std::string(LeafNameOfKey(cf.name));

		auto *pFunc = new SnFunction(FA_Public, NF_Data, pRetType, pName,
			pParams, loc);
		//R6-2: explicitly add NF_Imported. SnFunction's constructor does
		//not auto-set this flag (unlike SnIdentifierExpr). Without it,
		//RegisterStructs/Classes/.../PopulateClassMethods' IsImported()
		//gates would not skip these stubs.
		pFunc->AddFlags(NF_Imported);

		return pFunc;
	}

	//Build a SnStructDecl stub for an imported CompiledStruct.
	//Members are placeholders — VmBackend.RegisterStructs skips imported
	//stubs entirely (R3 + IsImported gating). Only the name matters for
	//name-based lookup from user code (e.g. `importedStruct` field types).
	SnStructDecl *CreateStructStub(const CompiledStruct &cs,
		const ISourceLocation &loc)
	{
		//Members intentionally empty — type/field info stays in the
		//loaded CompiledModule, which rides along to the load-time linker
		//as a peer image. The AST stub only needs to exist as a
		//name-resolution target.
		//SnStructDecl takes UniquePtrList<SnStructField> by value; pass a
		//raw PtrList* whose ownership is transferred via the implicit
		//inner_collection* constructor.
		auto *pMembers = new PtrList<SnStructField>();
		auto pName = new std::string(LeafNameOfKey(cs.name));
		auto *pDecl = new SnStructDecl(pName, pMembers, loc);
		pDecl->AddFlags(NF_Imported);   //R6-2
		return pDecl;
	}

	//Build a SnClassDecl stub for an imported CompiledClass.
	//Super-class and field/method members are placeholders; VmBackend
	//skips imported class registration (RegisterClasses/PopulateClassMethods).
	SnClassDecl *CreateClassStub(const CompiledClass &cc,
		const ISourceLocation &loc)
	{
		//Super-class name not reconstructed — imported class metadata
		//(superClassIdx etc.) stays in the loaded CompiledModule and is
		//resolved by the load-time linker from the peer images. AST stub
		//exists only for name resolution.
		auto pName = new std::string(LeafNameOfKey(cc.name));
		auto *pMembers = new PtrList<SnField>();
		auto *pDecl = new SnClassDecl(pName, nullptr, pMembers, loc);
		pDecl->AddFlags(NF_Imported);   //R6-2
		return pDecl;
	}

	//Layer 6 policy: imported module must not define main().
	//Phase 5: the entry judgment uses the qualified key — a root main.n
	//in the imported module is keyed "<module>.main", so the bare "main"
	//comparison would miss it.
	void CheckNoMainFunction(const CompiledModule &cm)
	{
		const std::string entryKey = cm.name + ".main";
		for (const auto &cf : cm.functions)
		{
			if (cf.name == entryKey)
			{
				throw std::runtime_error(
					"imported module '" + cm.name +
					"' must not define main(); main() is reserved for the root module");
			}
		}
	}

	SyntaxTree &m_Tree;
	std::string m_moduleName;
	std::vector<SnFunction *> m_importedFuncs;
	std::vector<SnField *> m_importedTypes;
	//Registered stubs that did NOT join the root (R10-1 collisions);
	//owned here until the caller takes them via TakeDetachedStubs().
	std::vector<std::unique_ptr<SnFunction>> m_detachedStubs;

	//Shortcut to the root namespace (avoids repeated m_Tree.Root() calls
	//in inner loops).
	SnNamespace &TheRoot() { return *m_Tree.Root(); }
};

} //namespace nlang
