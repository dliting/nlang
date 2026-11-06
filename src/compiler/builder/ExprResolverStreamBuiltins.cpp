/*---
    ExprResolverStreamBuiltins.cpp — the stream-method resolve stage:
    the serialization specials of ByteStream/FileStream. Since v0.12.7
    Task 8.5 the classes are declared in stdlib/io.n, so every other
    read/write/control method resolves through its declared signature;
    what remains here are the four cases the declaration syntax cannot
    express: readStruct/readObject's literal type argument with the
    return-type rebind, and writeStruct/writeObject's polymorphic
    argument gates.
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/vm/CompiledModule.h>  //RTK_* kind constants
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//ReadStruct("TypeName")/ReadObject("TypeName") type-name argument: the
//argument MUST be a string literal so the type resolves at compile time
//(variables rejected — no generics in NLang). The lookup walks the CALLER's
//namespace chain, NOT m_pContext — that is the io-declared stream class,
//whose declaration scope is not where user structs/classes live.
//Returns nullptr after logging (context restored); readObject's declared
//type may be a base class of the stream's actual type — polymorphic
//deserialization is enforced in VmExecutor via IsSubclassOf (Phase 8d).
SnField *ExprResolveAccessor::ResolveStreamSpecialTypeArg(
	SnInvokeExpr &invoke, NodeKind wantKind, const char *pMethodDisp,
	SyntaxNode *pSavedContext)
{
	auto& params = invoke.Params();
	auto it = params.begin();
	if (it == params.end() || (*it).Kind() != NK_LiteralExpr
		|| !(*it).EvalDataType()
		|| (*it).EvalDataType()->Kind() != NK_String)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"%s requires a string literal argument.", pMethodDisp);
		m_pContext = pSavedContext;
		return nullptr;
	}
	auto& lit = static_cast<SnLiteralExpr&>(*it);
	const std::string* pTypeName = lit.Value().Data().m_String;
	const std::string typeName = pTypeName ? *pTypeName : std::string();
	//Phase 5 D10: the literal resolves to a declaration NOW, from the
	//visible packages (own package + imported ones), and the string that
	//reaches the VM is rewritten to that declaration's table key. No
	//runtime bare-name fallback: the type tables are qualified-keyed.
	//The collection surface is the registry's visible-module set, not
	//FindField up the chain (import never adds a chain node, D2).
	SnField* found = nullptr;
	std::string foundKey;
	size_t hits = 0;
	{
		auto& reg = m_Env.Registry();
		//Visible modules: the caller's own package plus every module its
		//import gate opened. A dotted literal names its package in the
		//first segments, so only that module can own it; a bare literal
		//competes across ALL visible modules (ambiguity surface).
		const bool dotted = typeName.find('.') != std::string::npos;
		const size_t lastDot = typeName.find_last_of('.');
		const std::string prefix =
			dotted ? typeName.substr(0, lastDot) : std::string();
		const std::string leaf =
			dotted ? typeName.substr(lastDot + 1) : typeName;
		const uint32_t curModule = reg.OwnerOfContext(*pSavedContext);
		std::vector<std::string> seenKeys;
		for (uint32_t i = 0; i < reg.ModuleCount(); ++i)
		{
			const std::string& path = reg.ModulePathOf(i);
			//Candidate enumeration through the single-point import-gate
			//policy (ModuleRegistry::ImportGateAllows): an external owner
			//(stub context) sees every module — its references were
			//validated at the producer's own compile; the hits below still
			//pin the type identity (kind + dedup + ambiguity check).
			if (i != curModule && !reg.ImportGateAllows(curModule, path))
				continue;
			if (dotted && path != prefix)
				continue;
			SnField* hit = reg.FindModuleType(path, leaf);
			if (!hit || hit->Kind() != wantKind)
				continue;
			const std::string key = reg.QualifiedName(*hit);
			//A hit already seen (same qualified key) is the same type,
			//not a second candidate — the per-package duplicate is
			//rejected earlier by the name-conflict checks.
			if (std::find(seenKeys.begin(), seenKeys.end(), key)
				!= seenKeys.end())
				continue;
			seenKeys.push_back(key);
			found = hit;
			foundKey = key;
			++hits;
		}
	}
	if (hits > 1) {
		m_Env.Log(CLL_Error, invoke.Location(),
			"%s: type name '%s' is ambiguous (%zu visible types share it); "
			"qualify it (e.g. 'pkg.%s').", pMethodDisp, typeName.c_str(),
			hits, typeName.c_str());
		m_pContext = pSavedContext;
		return nullptr;
	}
	if (!found) {
		m_Env.Log(CLL_Error, invoke.Location(),
			"%s type not found: %s.", pMethodDisp, typeName.c_str());
		m_pContext = pSavedContext;
		return nullptr;
	}
	lit.RewriteStringValue(foundKey);   //table key, spelled by the registry
	m_pContext = pSavedContext;
	return found;
}

//The four serialization specials. The declared io.n signature carries
//Object params/returns (the wire shape); these gates keep the value
//semantics the declaration cannot express: writeStruct takes exactly a
//struct value, writeObject a class value, an array value, an interface
//reference or null (an interface slot serializes the actual object; an
//array slot serializes by logical content — the runtime dispatches the
//kind, so a bare array value can never reach the class-field walk).
//Null is Int32-typed on the wire; only writeObject takes it (a null
//object reference serializes as the null marker).
static const char* StreamSpecialArgExpectation(const std::string &name,
	SnExpression &arg, bool nullLit)
{
	auto* pArgType = arg.EvalDataType();
	NodeKind ak = pArgType ? pArgType->Kind()
		: static_cast<NodeKind>(-1);
	if (name == "writeStruct")
		return (!nullLit && ak == NK_StructDecl) ? nullptr : "a struct";
	if (name == "writeObject")
	{
		bool ok = nullLit || ak == NK_ClassDecl || ak == NK_InterfaceDecl
			|| ak == NK_ArrayTypeToken;
		return ok ? nullptr : "a class or array";
	}
	return nullptr;
}

//Stream serialization specials: ByteStream/FileStream member calls that
//resolve by name because their semantics outrun the declared signature.
//The declared classes live in stdlib/io.n (Task 8.5); every other
//method name — the scalar readers/writers and the stream-control pair
//— declines here and resolves through the ordinary declared-signature
//path like any native class member.
bool ExprResolveAccessor::TryResolveStreamBuiltinMethod(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& IsStreamClassName(
			static_cast<SnClassDecl*>(m_pContext)->Name())
		&& pInnerExpr->Kind() == NK_InvokeExpr))
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	const auto& name = invoke.CalleeName();
	if (name == "readStruct" || name == "readObject")
	{
		//Returns a value of the literal-named type — the declared
		//Object return is rebound to the declaration here.
		SnField* pFound = ResolveStreamSpecialTypeArg(invoke,
			name == "readStruct" ? NK_StructDecl : NK_ClassDecl,
			name == "readStruct" ? "ReadStruct" : "ReadObject",
			pSavedContext);
		if (!pFound)
			return true;
		//Bind m_pField (not just EvalDataType) so a chained member
		//survives IsDataExpr() — see BindTableStringMethodResult.
		snMember.EvalDataType(pFound);
		snMember.m_pField = pFound;
		ResolveStreamMethodTail(snMember, pInnerExpr, invoke, name,
			NK_Void, pSavedContext);
		return true;
	}
	if (name == "writeStruct" || name == "writeObject")
	{
		ResolveStreamMethodTail(snMember, pInnerExpr, invoke, name,
			NK_Void, pSavedContext);
		return true;
	}
	return false;   //ordinary method — declared-signature resolution
}

//Stream special resolve tail: by-name rejection (named/out arguments
//cannot bind by-name dispatch), caller-scope arg resolution, and the
//struct/object argument gate. The ordinary methods never pass through
//here (their signature checks ride the declared-parameter binding).
void ExprResolveAccessor::ResolveStreamMethodTail(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke, const std::string &name,
	NodeKind retKind, SyntaxNode *pSavedContext)
{
	(void)retKind;   //readStruct/readObject bound theirs above; writers are void
	//Round-12/14: by-name dispatch cannot bind named args; out args
	//cannot write back.
	if (RejectNamedOrOutArguments(invoke))
	{
		m_pContext = pSavedContext;
		return;
	}
	//Resolve the args so each param's Field()/EvalDataType() is
	//populated, then gate the serialization specials' value kinds.
	//Args are evaluated in the CALLER's scope: restore m_pContext AND
	//clear ERF_SearchInParentOnly (set above for the member lookup) so
	//a normal scope walk finds the caller's locals.
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	const size_t argCount = ArgCountOf(invoke);
	if (argCount != 1)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"stream method \"%s\" expects one argument.", name.c_str());
	}
	else if (name == "writeStruct" || name == "writeObject")
	{
		auto& arg = *invoke.Params().begin();
		if (arg.IsResolved())
		{
			const char* want = StreamSpecialArgExpectation(name,
				arg, arg.ContainFlags(NF_NullLiteral));
			if (want)
				m_Env.Log(CLL_Error, invoke.Location(),
					"stream method \"%s\" expects %s argument.",
					name.c_str(), want);
		}
	}
	pInnerExpr->AddFlags(NF_Resolved);
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
}

} //namespace nlang
