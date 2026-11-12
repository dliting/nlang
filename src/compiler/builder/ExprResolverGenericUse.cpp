/*--- ExprResolverGenericUse.cpp — 泛型使用的解析：Name<Args> 引用的
    实参收集、实参域检查与合成func/声明模板的路由。
    从 ExprResolverTypes.cpp 抽取（文件规模守卫驱动的拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
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
//under the bare name answers; a TYPE that is not a generic declaration
//is reported back for the miss diagnostics; anything else (including no
//name at all) leaves both outputs null.
SnClassDecl *ExprResolveAccessor::FindDeclaredGenericTemplate(
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
	//the head's kind picks the fix: a function head means explicit type
	//arguments on a call, and calls carry none at all (inference reads
	//the actuals); any other value head gets the parenthesized form.
	auto *pValueHead = m_pContext
		? FindFieldInAncestor(baseName, *m_pContext, *m_pAccessor,
			Flags())
		: nullptr;
	if (pValueHead)
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
		m_Env.Log(CLL_Error, genType.Location(),
			"The name \"%s\" resolves to a value, not a generic type: "
			"this shape reads as a generic instantiation; if you meant "
			"comparisons as arguments, parenthesize them, for example: "
			"f((a < b), (c > d)).",
			baseName.c_str());
		return false;
	}
	//D10: "not a built-in generic type" misled once the miss was merely
	//an unimported package class — state the miss plainly, then let the
	//package note name the fix when one exists.
	m_Env.Log(CLL_Error, genType.Location(),
		"Cannot resolve the type: \"%s\".", baseName.c_str());
	MaybeLogPackageHint(baseName);
	return false;
}

//Access(SnGenericTypeExpr&)'s declared-template arm (`class Box<T>` +
//`Box<int>`): the base must name a generic-declaration class in scope;
//the arity check runs against its parameter list; the instantiation is
//minted (cached) and bound. False = a diagnostic is logged.
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
	const size_t expected = pTemplate->TypeParamNames().size();
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
	if (RejectMultiSlotTypeArgs(m_Env, *pTemplate, typeArgs, baseName,
			genType.Location()))
		return false;
	auto *pInstance = GetDeclaredGenericClassDecl(*pTemplate, typeArgs);
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
