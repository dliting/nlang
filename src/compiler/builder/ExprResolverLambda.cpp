/*---
    ExprResolverLambda.cpp — lambda expression resolution (0.13.10).
    The cover grammar delivers every list item that is not a parameter
    shape as a raw expression; the parameter gate here rejects those by
    name (spec §3.5). Typing through the expected-type channel lands in
    the same entry (Task 4 of the lambda/closure plan).
---*/
#include "ExprResolver.h"
#include "BuildEnvironment.h"

namespace nlang
{

//True when any list item arrived as a raw expression. The parser's
//arrow-confirmed fold reorganizes bare identifiers and reinterprets
//the `int& x` spelling into a typed parameter; deeper bit-and chains
//and every other expression shape stay raw (Task 2 rulings) and are
//rejected here.
bool InvalidLambdaParameterList(SnLambdaExpr &sn)
{
	for (const auto &param : sn.Params())
		if (ParamIsRawExpression(param))
			return true;
	return false;
}

//0.13.10 Task 3: the lambda resolution entry. The gate reports the
//first offending item only and flags the node resolved so a decomposed
//local-decl revisit cannot re-report (the double-report discipline of
//the local-init diagnostics). Valid shapes stay untyped until the
//expected-type channel lands (Task 4); interim parse-only contract
//holds for them.
void ExprResolveAccessor::Access(SnLambdaExpr &sn)
{
	if (sn.IsResolved())
	{
		return;
	}

	if (InvalidLambdaParameterList(sn))
	{
		for (const auto &param : sn.Params())
		{
			if (ParamIsRawExpression(param))
			{
				m_Env.Log(CLL_Error, sn.Location(),
					"invalid lambda parameter list; \"%s\" is not a "
					"parameter.",
					param.pValueExpr->ToString().c_str());
				break;
			}
		}
		sn.AddFlags(NF_Resolved);
		return;
	}
}

} //namespace nlang
