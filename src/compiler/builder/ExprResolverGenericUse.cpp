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
#include "VmBackend.h"   //IsBuiltinHandleDeclName (D11 shared predicate)
#include <nlang/runtime/BuiltinGenericNames.h>
#include <vector>

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
//user template's T-typed field is a bare value slot — struct and
//function arguments are multi-slot values and are rejected there (a
//documented staging limitation; interfaces stay allowed: an interface
//value is a single-slot object handle). True = rejected and logged.
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
		if (pArg->Kind() == NK_StructDecl)
		{
			env.Log(CLL_Error, pLoc,
				"the struct \"%s\" cannot be a type argument of "
				"\"%s\": a struct is a multi-slot value, and a "
				"generic field stores one value slot.",
				pArg->Name().c_str(), baseName.c_str());
			return true;
		}
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

//Phase 13 Step 2: Dict keyed by a func type — DictKeysEqual is identity
//for func records (no interning), so two references to the same function
//would store as two entries. True = rejected and logged.
static bool RejectFuncDictKey(BuildEnvironment &env,
	const std::vector<SnField*> &typeArgs, const std::string &baseName,
	const ISourceLocation *pLoc)
{
	if (baseName != kBuiltinDictTypeName || typeArgs.empty()
		|| !IsFuncTypeDecl(typeArgs[0]))
		return false;
	env.Log(CLL_Error, pLoc,
		"function types cannot be used as Dict keys.");
	return true;
}

//Access(SnGenericTypeExpr&)'s declared-template arm (`class Box<T>` +
//`Box<int>`): the base must name a generic-declaration class in scope;
//the arity check runs against its parameter list; the instantiation is
//minted (cached) and bound. False = a diagnostic is logged.
bool ExprResolveAccessor::TryResolveDeclaredGenericInstance(
	SnGenericTypeExpr &genType, const std::string &baseName)
{
	//The base must name a generic-declaration class in scope.
	SnClassDecl *pTemplate = nullptr;
	if (m_pContext)
	{
		auto *pProbed = FindTypeNameInAncestor(baseName, *m_pContext,
			*m_pAccessor, false);
		if (pProbed && pProbed->Kind() == NK_ClassDecl
			&& static_cast<SnClassDecl*>(pProbed)->IsGenericDeclaration())
			pTemplate = static_cast<SnClassDecl*>(pProbed);
	}
	if (!pTemplate)
	{
		//D10: "not a built-in generic type" misled once the miss was
		//merely an unimported package class — state the miss plainly,
		//then let the package note name the fix when one exists.
		m_Env.Log(CLL_Error, genType.Location(),
			"Cannot resolve the type: \"%s\".", baseName.c_str());
		MaybeLogPackageHint(baseName);
		return false;
	}
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
	if (RejectFuncDictKey(m_Env, typeArgs, baseName, genType.Location()))
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
	assert(pBase);
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
