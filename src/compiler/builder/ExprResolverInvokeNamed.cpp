/*--- ExprResolverInvokeNamed.cpp — the name-shape channel of
    Access(SnInvokeExpr&): overload lookup by callee name with its
    chosen-callee gates and success/failure tails. Split from
    ExprResolverInvoke.cpp by the source-size guard (v0.13.13a D-3),
    zero behavior change; per-concern-TU precedent
    ExprResolverInvokeMatch.cpp.
---*/
#include "ExprResolver.h"
#include "RefBindJudgment.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <nlang/runtime/StackDump.h>

namespace nlang

{

//v0.13.13a D-3: the name-shape channel of Access(SnInvokeExpr&).
//The postfix callee-expression shape never reaches it (an empty
//CalleeName would read as a not-found). The block runs verbatim;
//the deferred-argument sweep rides in through deferredArgs.
void ExprResolveAccessor::ResolveNameShapeInvoke(SnInvokeExpr &snInvoke,
	std::vector<SnExpression*> &deferredArgs)
{
	//The moved block starts here.
	SnFunction *pCallee;
	std::vector<FormalBinding> bindings;
	std::vector<SnFunction*> candidates;
	bool bNameMatchedImported = false;
	bool bVisibilityHintLogged = false;
	bool bAmbiguousCall = false;
	auto res = FindFuncByInvoke(pCallee, snInvoke, bindings,
		bNameMatchedImported, bVisibilityHintLogged, &candidates,
		&bAmbiguousCall);

	if (!AdmitChosenCallee(snInvoke, pCallee, bindings, deferredArgs))
		return;

	if (res == FFR_FuncNameNotFound && RejectCrossNamespaceCall(snInvoke))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return;
	}

	if (res == FFR_ExactMatch || res == FFR_ApproximateMatch)
	{
		//M3b: the success tail is shared with the module-qualified call
		//path. A false return means an argument failed to bind (diagnostic
		//already logged) — leave the invoke unresolved.
		(void)ResolveInvokeWithFunc(snInvoke, *pCallee, res, bindings);
		return;
	}

	ResolveFailedInvoke(snInvoke, res, pCallee, bNameMatchedImported,
		bVisibilityHintLogged, candidates, deferredArgs,
		bAmbiguousCall);
}

} //namespace nlang
