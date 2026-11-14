/*--- ExprResolverGenericUse.cpp — 泛型使用的解析：Name<Args> 引用的
    实参收集、实参域检查与合成func/声明模板的路由。
    从 ExprResolverTypes.cpp 抽取（文件规模守卫驱动的拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "ScriptParser.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include "ModuleBuilderChecks.h"
#include "BindingTable.h"   //IsBuiltinHandleDeclName (D11)
#include <nlang/runtime/BuiltinGenericNames.h>
#include <vector>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Type-argument collection of Access(SnGenericTypeExpr&): resolves each
//argument in the caller's scope and gathers the canonical fields plus
//the parallel out flags. False = a diagnostic is logged (unresolvable
//argument, argument without a field, or the reject gate above).
bool ExprResolveAccessor::TryResolveGenericTypeArgs(
	SnGenericTypeExpr &genType, const std::string &baseName,
	std::vector<SnField*> &typeArgs, std::vector<uint8> &outFlags)
{
	for (auto *pTA : genType.TypeArgs())
	{
		if (!pTA) continue;
		pTA->Accept(*m_pVisitor);
		if (!pTA->IsResolved())
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"Cannot resolve type argument %s.",
				pTA->ToString().c_str());
			return false;
		}
		auto *pField = pTA->Field();
		if (!pField)
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"Type argument %s has no resolved field.",
				pTA->ToString().c_str());
			return false;
		}
		if (RejectInvalidTypeArg(m_Env, pTA, baseName, typeArgs.empty()))
			return false;
		//v0.12.2 face gate: a type argument is a type position too —
		//face classes (`List<String>`) have no element layout. One
		//throat for every arm: the built-in generics, the declared
		//templates, and the Func mint all resolve through here.
		if (RejectPreludeFaceUse(m_Env, pField, pTA->Location(),
			"used as a type"))
			return false;
		//0.7.3 B: the resolved field above IS the type identity — an
		//array-typed argument carries the interned SnArrayTypeToken
		//(same intern channel as declarations), so the key, the display
		//name and every consumer read array-ness off the token itself.
		typeArgs.push_back(pField);
		outFlags.push_back(pTA->ContainFlags(NF_Out) ? 1 : 0);
	}
	return true;
}

//v0.12.3 dictionary model (spec D2 argument domain): the builtin
//containers box multi-slot values inside their element storage, but a
//user template's T-typed field is a bare value slot — function
//arguments are multi-slot records and are rejected there (interfaces
//stay allowed: an interface value is a single-slot object handle).
//v0.13.0 D3: struct arguments are admitted — every boundary
//(call formals, local binds, external field stores, foreach) deep-
//copies the record through the substituted type, so the field's slot
//holds one private handle. True = rejected and logged.
static bool RejectMultiSlotTypeArgs(BuildEnvironment &env,
	const SnClassDecl &tmpl, const std::vector<SnField*> &typeArgs,
	const std::string &baseName, const ISourceLocation *pLoc)
{
	const uint32_t templateOwner = env.Registry().OwnerOf(tmpl);
	//D11: the builtin templates are exempt wherever they are declared —
	//prelude-owned, or a stamped package's handle class after the
	//concurrency classes leave the prelude (Channel boxes multi-slot
	//element values inside its storage either way).
	if (templateOwner == ModuleRegistry::NO_OWNER
			|| IsBuiltinHandleDeclName(env.Registry(), templateOwner,
				tmpl.Name()))
		return false;
	for (auto* pArg : typeArgs)
	{
		if (IsFuncTypeDecl(pArg))
		{
			env.Log(CLL_Error, pLoc,
				"function types cannot be type arguments of "
				"\"%s\": a function value is a multi-slot record "
				"(target, receiver, form), and a generic field "
				"stores one value slot.",
				baseName.c_str());
			return true;
		}
	}
	return false;
}

//The declared-template arm's scope probe: a generic-declaration class
//or interface (0.13.1c) under the bare name answers; a TYPE that is
//not a generic declaration is reported back for the miss diagnostics;
//anything else (including no name at all) leaves both outputs null.
SyntaxNode *ExprResolveAccessor::FindDeclaredGenericTemplate(
	const std::string &baseName, SnField **ppNonGenericType)
{
	*ppNonGenericType = nullptr;
	if (!m_pContext)
		return nullptr;
	auto *pProbed = FindTypeNameInAncestor(baseName, *m_pContext,
		*m_pAccessor, false);
	if (pProbed && pProbed->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(pProbed)->IsGenericDeclaration())
		return static_cast<SnClassDecl*>(pProbed);
	if (pProbed && pProbed->Kind() == NK_InterfaceDecl
		&& static_cast<SnInterfaceDecl*>(pProbed)->IsGenericDeclaration())
		return static_cast<SnInterfaceDecl*>(pProbed);
	*ppNonGenericType = pProbed;
	return nullptr;
}

//The declared-template arm's miss diagnostics. The spelling probe is a
//shape rule (the C# one), so three situations funnel here: a live type
//that is not generic; a value/function head whose call collided with
//the shape (`f(a < b, c > (d))` reads as `a<b, c>(d)`); and the plain
//unknown name. Always returns false.
bool ExprResolveAccessor::LogDeclaredGenericMiss(SnGenericTypeExpr &genType,
	const std::string &baseName, SnField *pNonGenericType)
{
	//A type that takes no type arguments says so directly.
	if (pNonGenericType)
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"The type \"%s\" is not generic: it takes no type arguments.",
			baseName.c_str());
		return false;
	}
	//The head names a VALUE, so the shape collision is what happened;
	//the head's kind picks the fix (function vs other value).
	auto *pValueHead = m_pContext
		? FindFieldInAncestor(baseName, *m_pContext, *m_pAccessor,
			Flags())
		: nullptr;
	if (pValueHead)
		return LogValueHeadMiss(genType, baseName, pValueHead);
	//D10: "not a built-in generic type" misled once the miss was merely
	//an unimported package class — state the miss plainly, then let the
	//package note name the fix when one exists.
	m_Env.Log(CLL_Error, genType.Location(),
		"Cannot resolve the type: \"%s\".", baseName.c_str());
	MaybeLogPackageHint(baseName);
	return false;
}

//The value-head arm of LogDeclaredGenericMiss: a function head says
//calls carry no type-argument spelling; any other value head tries the
//0.13.1c comparison rescue first and names the parenthesized fix on
//failure. Always returns false.
bool ExprResolveAccessor::LogValueHeadMiss(SnGenericTypeExpr &genType,
	const std::string &baseName, SnField *pValueHead)
{
	if (pValueHead->Kind() == NK_Function)
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"The name \"%s\" resolves to a function, not a generic "
			"type: this shape reads as a generic instantiation; a "
			"call carries no type-argument spelling — write the "
			"call without it.",
			baseName.c_str());
		return false;
	}
	//0.13.1c identity rescue: the head is a value, so the type
	//reading is falsified — try the comparison re-read first; on
	//failure fall through to the named diagnostic unchanged.
	if (TryRescueComparisonReading(genType))
		return false;
	m_Env.Log(CLL_Error, genType.Location(),
		"The name \"%s\" resolves to a value, not a generic type: "
		"this shape reads as a generic instantiation; if you meant "
		"comparisons as arguments, parenthesize them, for example: "
		"f((a < b), (c > d)).",
		baseName.c_str());
	return false;
}

//Shape discovery for the rescue: the misread call must sit directly in
//an argument list — the generic head's parent is the minting SnNewExpr
//and that node's parent is the call (or outer creation) whose argument
//list splices. Returns the misread node and sets \p ppList to the
//argument list, or returns null when the shape does not splice.
static SnNewExpr *FindRescuableMisread(SnGenericTypeExpr &genType,
	SnExpressionList *&ppList)
{
	auto *pParent = genType.Parent();
	if (!pParent || pParent->Kind() != NK_NewExpr)
		return nullptr;
	auto *pMisread = static_cast<SnNewExpr *>(pParent);
	if (pMisread->ClassName() != &genType)
		return nullptr;
	auto *pOwner = pMisread->Parent();
	if (!pOwner)
		return nullptr;
	if (pOwner->Kind() == NK_InvokeExpr)
		ppList = &static_cast<SnInvokeExpr *>(pOwner)->Params();
	else if (pOwner->Kind() == NK_NewExpr)
		ppList = &static_cast<SnNewExpr *>(pOwner)->Args();
	return ppList ? pMisread : nullptr;
}

//0.13.1c identity rescue: the head resolved to a value, so the type
//reading of `a<b, c>(d)` is falsified and the comparison reading is the
//only sound one (the name-partition invariant). Re-parse the span's
//verbatim source bytes in forced comparison reading. True = splice
//staged for the owning list's exit; false = fall back to the 0.13.1b
//diagnostic unchanged.
bool ExprResolveAccessor::TryRescueComparisonReading(SnGenericTypeExpr &genType)
{
	SnExpressionList *pList = nullptr;
	auto *pMisread = FindRescuableMisread(genType, pList);
	if (!pMisread)
		return false;

	//Span text: the verbatim source bytes captured at reduce time
	//(SnNewExpr::SpanText) — an AST re-render would compress whitespace
	//and break the fragment-to-file column correspondence, and multi-line
	//spans would lose their line structure. No captured bytes (a
	//different minting arm) means no rescue.
	const std::string &sSpan = pMisread->SpanText();
	if (sSpan.empty())
		return false;

	auto *pSpanStart = static_cast<const ScriptLocation *>(
		genType.Location());
	NLANG_ASSERT(pSpanStart);
	std::vector<SnExpression*> rescued;
	if (!ScriptParser::ParseFragment(sSpan, *pSpanStart, rescued))
	{
		//The span is not well-formed comparisons (an empty argument
		//list, a stray token shape). Discard whatever parsed and let
		//the caller log the 0.13.1b diagnostic unchanged.
		for (auto *pArg : rescued)
			DeleteNode(pArg);
		return false;
	}

	//Wait for the owning list's exit — splicing mid-pass would mutate
	//a list whose iterators are live (ResolveExpressionList range-for).
	RescueStash stash;
	stash.pList = pList;
	stash.pMisread = pMisread;
	stash.rescued = std::move(rescued);
	m_rescueStash.push_back(std::move(stash));
	return true;
}

//The declared-template arm's interface face (`interface I<T>` +
//`I<Args>`): implements entries and every expression-position use share
//the one Access throat, so the implements clause binds the INSTANTIATED
//interface and the completeness check reads substituted signatures.
//False = a diagnostic is logged.
bool ExprResolveAccessor::TryResolveDeclaredGenericInterfaceInstance(
	SnGenericTypeExpr &genType, const std::string &baseName,
	SnInterfaceDecl &tmpl)
{
	const size_t expected = tmpl.TypeParamNames().size();
	if (genType.TypeArgs().size() != expected)
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"The generic interface \"%s\" expects %zu type "
			"argument(s), but got %zu.",
			baseName.c_str(), expected, genType.TypeArgs().size());
		return false;
	}
	std::vector<SnField*> typeArgs;
	std::vector<uint8> outFlags;
	//No multi-slot gate here: an interface carries no value storage —
	//its T-typed members are signature positions only, so every
	//argument kind is a legal substitution.
	if (!TryResolveGenericTypeArgs(genType, baseName, typeArgs, outFlags))
		return false;
	auto *pInstance = GetDeclaredGenericInterfaceDecl(tmpl, typeArgs);
	if (!pInstance)
		return false;
	ResolveFieldExprAs(genType, pInstance);
	return true;
}

//Access(SnGenericTypeExpr&)'s declared-template arm (`class Box<T>` +
//`Box<int>`): the base must name a generic-declaration class or
//interface in scope; the arity check runs against its parameter list;
//the instantiation is minted (cached) and bound. False = a diagnostic
//is logged.
bool ExprResolveAccessor::TryResolveDeclaredGenericInstance(
	SnGenericTypeExpr &genType, const std::string &baseName)
{
	//Every answer other than a declared generic template is diagnosed
	//by the miss path.
	SnField *pNonGenericType = nullptr;
	auto *pTemplate = FindDeclaredGenericTemplate(baseName,
		&pNonGenericType);
	if (!pTemplate)
		return LogDeclaredGenericMiss(genType, baseName, pNonGenericType);
	if (pTemplate->Kind() == NK_InterfaceDecl)
		return TryResolveDeclaredGenericInterfaceInstance(genType,
			baseName, *static_cast<SnInterfaceDecl*>(pTemplate));
	auto *pClassTemplate = static_cast<SnClassDecl*>(pTemplate);
	const size_t expected = pClassTemplate->TypeParamNames().size();
	if (genType.TypeArgs().size() != expected)
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"The generic class \"%s\" expects %zu type argument(s), "
			"but got %zu.",
			baseName.c_str(), expected, genType.TypeArgs().size());
		return false;
	}
	std::vector<SnField*> typeArgs;
	std::vector<uint8> outFlags;
	if (!TryResolveGenericTypeArgs(genType, baseName, typeArgs, outFlags))
		return false;
	if (RejectMultiSlotTypeArgs(m_Env, *pClassTemplate, typeArgs, baseName,
			genType.Location()))
		return false;
	auto *pInstance = GetDeclaredGenericClassDecl(*pClassTemplate,
		typeArgs);
	if (!pInstance)
		return false;
	ResolveFieldExprAs(genType, pInstance);
	return true;
}

void ExprResolveAccessor::Access(SnGenericTypeExpr &genType)
{
	if (genType.IsResolved())
		return;

	auto *pBase = genType.Base();
	NLANG_ASSERT(pBase);
	std::string baseName = pBase->ToString();
	//D4 routing split: only func mints synthetically now — the four
	//generic prelude names resolve through their declared templates
	//(Promise precedent). IsBuiltinGenericTypeName remains the
	//SCANNER's flag set only.
	if (baseName != kBuiltinFuncTypeName)
	{
		TryResolveDeclaredGenericInstance(genType, baseName);
		return;
	}

	std::vector<SnField*> typeArgs;
	std::vector<uint8> outFlags;
	if (!TryResolveGenericTypeArgs(genType, baseName, typeArgs, outFlags))
		return;

	auto *pSynClass =
		GetGenericClassDecl(m_Env.Registry(), baseName,
			typeArgs, outFlags,
			pBase->Location());
	if (!pSynClass)
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"Generic instantiation %s<...> is not supported in this phase.",
			baseName.c_str());
		return;
	}

	ResolveFieldExprAs(genType, pSynClass);
}

} //namespace nlang
