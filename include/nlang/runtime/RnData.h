#pragma once
#include "Variant.h"
#include "RuntimeNode.h"
#include "NodeContainers.h"
#include <memory>

namespace nlang
{

class RnFunction;

//The field contain typed data and a optional default value.
class NLANG_RUNTIME_API RnDataField : public RnField
{
	typedef RnField Super_;
public:
	static const NodeBits	s_DefaultFlags	= NF_Data | NF_Field;
public:
	RnDataField(NodeKind, FieldAccessType at, const IdString &name, 
		RnDataType &, void *pDefault);

	//Get the data type.
	RnDataType &Type()
	{
		return *m_Value.Type();
	}

	const RnDataType &Type() const
	{
		return *m_Value.Type();
	}

	//Get the default value.
	void *Value()
	{
		return m_Value.Data().m_Memory;
	}

	const void *Value() const
	{
		return m_Value.Data().m_Memory;
	}
protected:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	Variant m_Value;
};

//The formal parameter of a function.
class NLANG_RUNTIME_API RnFormalParam : public RnDataField
{
	typedef RnDataField Super_;
public:
	static const NodeKind	s_Kind			= NK_FormalParam;
	static const NodeBits	s_DefaultFlags	= Super_::s_DefaultFlags;
public:
	RnFormalParam(const IdString &name, RnDataType&, void *pDefault = nullptr);

	bool IsOptional() const
	{
		return ContainFlags(NF_Optional);
	}

	bool IsReference() const
	{
		return ContainFlags(NF_Reference);
	}
};


/*
A function field.
Each function has a name, a return type(could be "void") and optionally 
some parameters.
*/
class NLANG_RUNTIME_API RnFunction : public RnCompoundField
{
	typedef RnCompoundField Super_;
public:
	typedef ChildFieldList<RnFormalParam, BypassNodeFilter> ParamList;
	static const NodeKind			s_Kind			= NK_Function;
	static const NodeBits			s_DefaultFlags	= NF_Data | NF_Field;
public:
	explicit RnFunction(const IdString &name, RnField *pReturnType);

	~RnFunction() override;

	/*
	Does the given function conflict with this one?
	\param other The function to be compared with this one.
	\return true if it is in conflict with this one.
	*/
	bool ConflictedWith(const RnField &other) const override;

	//Get the params.
	//@{
	ParamList &Params()
	{
		return *m_upParams;
	}

	const ParamList &Params() const
	{
		return *m_upParams;
	}
	//@}

	//Get the constant return type.
	RnField *ReturnType() const
	{ 
		return m_pReturnType; 
	}

	void Accept(IRuntimeNodeVisitor&) override;

	bool CanAddChild(const RuntimeNode *pChild) const override;
private:
	RnField *m_pReturnType;
	std::unique_ptr<ParamList> m_upParams;
};

} //namespace nlang