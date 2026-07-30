/*-----------------------------------------------------------------------------
	nlang/intf/RnMisc.h
	This file defined the implementation of miscellaneous node class in nlang. 
-----------------------------------------------------------------------------*/

#include "RnMisc.h"
#include "RnData.h"
#include "RuntimeNodeVisitor.h"
#include "Runtime.h"

namespace nlang
{

RnNamespace::RnNamespace(const IdString &name) :
	Super_(s_Kind, FA_Public, name), m_upMembers(new MemberList(this))
{
}

RnNamespace::~RnNamespace()
{
	// m_upMembers is now unique_ptr - auto-deleted
}

void RnNamespace::Accept(IRuntimeNodeVisitor &v)
{
	v.Visit(*this);
}

bool RnNamespace::CanAddChild(const RuntimeNode *pChild) const
{
	assert(pChild);
	if (!pChild->IsField())
		return true;
	const RnField *pField = static_cast<const RnField*>(pChild);
	return !m_upMembers->FindConflicted(*pField);
}

} //namespace nlang