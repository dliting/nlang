/*-----------------------------------------------------------------------------
	ncomp/intf/SnTypes.h
	This file define the interface of data type syntax nodes in an nlang AST.
-----------------------------------------------------------------------------*/

#pragma once
#include "SyntaxNode.h"
#include <nlang/runtime/Flagable.h>
#include <nlang/runtime/RnTypes.h>

namespace nlang
{

//The syntax node of a primitive type in nlang.
class NLANG_COMPILER_API SnBuiltinDataType: public SnField
{
	typedef SnField Super_;
public:
	static SnBuiltinDataType  *InstanceOf(NodeKind builtinKind);

	std::string ToString() const override;

	SnField *FindField(const std::string& sName) const override;

	bool AllowPublicAccess(const SnField &accessor) const override;

	virtual SnField *EvalDataType() const override;
protected:
	ImmutableNodeList *ChildrenPtr() const override;

	explicit SnBuiltinDataType(RnBuiltinDataType &rtti);
};

template<class RTTI_T, class FIELD_T>
class SnBuiltinDataTypeT : public SnBuiltinDataType
{
	typedef SnBuiltinDataType Super_;
	friend class TreeBuildVisitorImpl;
public:
	typedef				RTTI_T				ImportInfo;
	typedef typename	ImportInfo::CppType		CppType;
	static const		NodeKind			s_Kind = ImportInfo::s_Kind;
public:
	~SnBuiltinDataTypeT() override
	{
		s_pInstance = nullptr;
	}

	static FIELD_T *Instance()
	{
		return s_pInstance;
	}
protected:
	SnBuiltinDataTypeT() : Super_(*ImportInfo::Instance())
	{
		assert(s_pInstance == nullptr);
		s_pInstance = static_cast<FIELD_T *>(this);
	}
private:
	static FIELD_T *s_pInstance;
};

template<class RTTI_T, class FIELD_T> 
FIELD_T *SnBuiltinDataTypeT<RTTI_T, FIELD_T>::s_pInstance = nullptr;

class NLANG_COMPILER_API SnInt32 : public SnBuiltinDataTypeT<RnInt32, SnInt32>
{
public:
	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor &) override;
};

class NLANG_COMPILER_API SnFloat : public SnBuiltinDataTypeT<RnFloat, SnFloat>
{
public:
	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor &) override;
};

class NLANG_COMPILER_API SnString : public SnBuiltinDataTypeT<RnString, SnString>
{
public:
	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor &) override;
};

class NLANG_COMPILER_API SnType : public SnBuiltinDataTypeT<RnType, SnType>
{
public:
	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor &) override;
};
} //namespace nlang
