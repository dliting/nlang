/*-----------------------------------------------------------------------------
	nlang/intf/RnMisc.h
	This file defined the interfaces of miscellaneous node class s in nlang. 
-----------------------------------------------------------------------------*/

#pragma once
#include "RnTypes.h"
#include "RnData.h"
#include "Variant.h"
#include "NodeContainers.h"
#include <vector>
#include <memory>


namespace nlang
{

//The type information of a namespace.
class NLANG_RUNTIME_API RnNamespace : public RnCompoundField
{
	typedef RnCompoundField Super_;
public:
	typedef ChildFieldList<RnField, BypassNodeFilter> MemberList;
	static const NodeKind s_Kind			= NK_Namespace;
	static const NodeBits s_DefaultFlags	= NF_Field;
public:
	explicit RnNamespace(const IdString &name);

	~RnNamespace() override;

	//Get the members of this node.
	//@{
	MemberList &Members()
	{
		return *m_upMembers;
	}

	const MemberList &Members() const
	{
		return *m_upMembers;
	}
	//@}

	//Is this namespace a global namespace?
	bool IsGlobal() const
	{
		return false;
	}

	virtual void Accept(IRuntimeNodeVisitor&) override;
protected:
	bool CanAddChild(const RuntimeNode *pChild) const override;
private:
	std::unique_ptr<MemberList> m_upMembers;
};

} //namespace nlang
