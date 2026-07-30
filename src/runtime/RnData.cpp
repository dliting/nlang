/*-----------------------------------------------------------------------------
	nlang/intf/DataFields.cpp
	This file defined the implementation of data fields in nlang. 
-----------------------------------------------------------------------------*/

#include "RnData.h"
#include "RuntimeNodeVisitor.h"
#include "Runtime.h"

namespace nlang
{

RnDataField::RnDataField(NodeKind kind, FieldAccessType at, 
	const IdString &name, RnDataType &type, void *pDefault) :
	Super_(kind, at, name), m_Value(type, pDefault)
{
}

ImmutableNodeList *RnDataField::ChildrenPtr() const
{
	return ImmutableNodeList::NullList();
}

RnFormalParam::RnFormalParam(const IdString &name, RnDataType &type, 
	void *pDefault /*= nullptr*/):
	Super_(s_Kind, FA_Private, name, type, pDefault)
{
}

bool RnFunction::ConflictedWith(const RnField &other) const
{
	if (other.Kind() != NK_Function)
		return Super_::ConflictedWith(other);

	//Else both are functions.
	if (other.Name() != Name())
		return false;

	const RnFunction &fOther = static_cast<const RnFunction&>(other);

	auto iOther		= fOther.Params().begin();
	auto iOtherEnd	= fOther.Params().end();
	auto iThis		= Params().begin();
	auto iThisEnd	= Params().end();
	//Compare the params one by one.
	while (true)
	{
		//Get the parameter pointer. 
		const RnFormalParam *pThis  = iThis == iThisEnd ? nullptr : &(*iThis);
		const RnFormalParam *pOther = iOther == iOtherEnd ? nullptr : &(*iOther);

		//If numbers of parameters are not equal.
		if (pThis && !pOther)
			return pThis->IsOptional();
		if (!pThis && pOther)
			return pOther->IsOptional();

		//Stop compare if both are optional parameters.
		if (pThis->IsOptional() && pOther->IsOptional())
			return true;

		//If parameter types are not the same.
		if (pOther->Type() != pThis->Type())
			return false;

		++iThis;
		++iOther;
	}
	return true;
}

RnFunction::RnFunction(const IdString &name, RnField *pReturnType) :
	Super_(s_Kind, FA_Default, name), m_pReturnType(pReturnType),
	m_upParams(new ParamList(this))
{
}

RnFunction::~RnFunction()
{
	// m_upParams is now unique_ptr - auto-deleted
}

void RnFunction::Accept(IRuntimeNodeVisitor &v)
{
	v.Visit(*this);
}

bool RnFunction::CanAddChild(const RuntimeNode *pChild) const
{
	return false;
}

} //namespace nlang