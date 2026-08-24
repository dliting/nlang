/*-----------------------------------------------------------------------------
	nlang/intf/RnTypes.h
	This file defined the interfaces of data type fields in nlang.
-----------------------------------------------------------------------------*/

#pragma once
#include "RuntimeNode.h"
#include <string>
#include <list>

namespace nlang
{

//The base class of all types those could act as value types.
class NLANG_RUNTIME_API RnDataType : public RnField
{
	typedef RnField Super_;
public:
	RnDataType(NodeKind, FieldAccessType, const IdString &name);

	virtual void InitValue(void *pDst, const void *pSrc) const = 0;

	virtual void CopyValue(void *pDst, const void *pSrc) const = 0;

	virtual void DestroyValue(void *pValue) const = 0;

	virtual std::string ValueToString(const void *pValue) const = 0;

	bool operator==(const RnDataType &rhs) const
	{
		return &rhs == this;
	}

	bool operator!=(const RnDataType &rhs) const
	{
		return !operator==(rhs);
	}
};

//The base class of built-in data type field.
class NLANG_RUNTIME_API RnBuiltinDataType : public RnDataType
{
	typedef RnDataType Super_;
public:
	static const NodeBits s_DefaultFlags = NF_Type | NF_Field | NF_Plain | NF_DontDelete;
public:
	RnBuiltinDataType(NodeKind kind, const char* szName, 
		const type_info &cppTypeId);

	//Get the c++ typeid of this type.
	const type_info &CppTypeId() const
	{
		return m_CppTypeId;
	}

	void InitValue(void *pDst, const void *pSrc) const override;

	void DestroyValue(void *pValue) const override;

	/*
	Get the type instance of a specified primitive kind.
	\return Return null if the give node kine not a primitive type.
	*/
	static RnBuiltinDataType *InstanceOf(NodeKind k);

	static void StaticInit();
protected:
	bool CanAddChild(const RuntimeNode *pChild) const override;

	ImmutableNodeList *ChildrenPtr() const override;

	static const char *NameOf(NodeKind);
private:
	const type_info &m_CppTypeId;
};

template<NodeKind KIND, class CPP_T>
class RnBuiltinDataTypeT : public RnBuiltinDataType
{
	typedef RnBuiltinDataType Super_;
public:
	typedef CPP_T			CppType;
	static const NodeKind	s_Kind = KIND;
	static const char*		s_szName;
public:
	void CopyValue(void *pDst, const void *pSrc) const override
	{
		assert(pDst && pSrc);
		*static_cast<CppType*>(pDst) = *static_cast<const CppType*>(pSrc);
	}
protected:
	RnBuiltinDataTypeT() :
		Super_(s_Kind, s_szName, typeid(CppType))
	{
	}
};

template<NodeKind KIND, class CPP_T>
const char* RnBuiltinDataTypeT<KIND, CPP_T>::s_szName =
	RnBuiltinDataType::NameOf(KIND);

//The 32-bit signed integer type.
class NLANG_RUNTIME_API RnInt32 : public RnBuiltinDataTypeT<NK_Int32, int32>
{
	typedef RnBuiltinDataTypeT<NK_Int32, int32> Super_;
public:
	static RnInt32 *Instance()
	{
		static RnInt32 s_Instance;
		return &s_Instance;
	}

	std::string ValueToString(const void *pValue) const override;

	void Accept(IRuntimeNodeVisitor &v) override;
};

//The 32-bit IEEE float type.
class NLANG_RUNTIME_API RnFloat : public RnBuiltinDataTypeT<NK_Float, float>
{
	typedef RnBuiltinDataTypeT<NK_Float, float> Super_;
public:
	static RnFloat *Instance()
	{
		static RnFloat s_Instance;
		return &s_Instance;
	}

	std::string ValueToString(const void *pValue) const override;

	void Accept(IRuntimeNodeVisitor &v) override;
};

//The string type.
class NLANG_RUNTIME_API RnString : public RnBuiltinDataTypeT<NK_String, std::string *>
{
	typedef RnBuiltinDataTypeT<NK_String, std::string *> Super_;
public:
	static RnString *Instance()
	{
		static RnString s_Instance;
		return &s_Instance;
	}

	void InitValue(void *pDst, const void *pSrc) const override;

	void CopyValue(void *pDst, const void *pSrc) const override;

	void DestroyValue(void *pValue) const override;

	std::string ValueToString(const void *pValue) const override;

	void Accept(IRuntimeNodeVisitor &v) override;
};

/*
The type information of a type.
The value of RnType is (RnField *).
*/
class NLANG_RUNTIME_API RnType : public RnBuiltinDataTypeT<NK_Type, RnField *>
{
	typedef RnBuiltinDataTypeT<NK_Type, RnField *> Super_;
public:
	static RnType *Instance()
	{
		static RnType s_Instance;
		return &s_Instance;
	}

	std::string ValueToString(const void *pValue) const override;

	void Accept(IRuntimeNodeVisitor &v) override;
};

/*
The void type, the type of "no value".
It only appears in the return slot of Func<...> (e.g. Func<void, int>).
No value of this type ever exists, so the value operations are no-ops.
*/
class NLANG_RUNTIME_API RnVoid : public RnBuiltinDataType
{
	typedef RnBuiltinDataType Super_;
public:
	//Mirror the RnBuiltinDataTypeT interface for the Sn mirror template.
	//CppType is void; no value of this type exists, so nothing ever
	//instantiates a member that would dereference it.
	typedef void			CppType;
	static const NodeKind	s_Kind = NK_Void;

	static RnVoid *Instance()
	{
		static RnVoid s_Instance;
		return &s_Instance;
	}

	void InitValue(void *pDst, const void *pSrc) const override;

	void CopyValue(void *pDst, const void *pSrc) const override;

	std::string ValueToString(const void *pValue) const override;

	void Accept(IRuntimeNodeVisitor &v) override;
protected:
	RnVoid();
};

///*
//The type of an object at runtime.
//Every C++ instance of Class delegates a script class in OOP. Every class 
//belong one namespace. Nested classes is not allowed now.
//*/
//class NLANG_RUNTIME_API Class: public FType, public RnMemberContainer<RnNamed>
//{
//	friend class Runtime;
//
//	struct VTable
//	{
//		void *m_pClass;
//		void* *m_pItems;
//	};
//
//	typedef FType Super_;
//public:
//	Class *Super() const
//	{
//		return m_pSuper;
//	}
//
//	virtual bool CanAddChild(const RuntimeNode *pChild) const override;
//
//	virtual void Accept(IRuntimeNodeVisitor&) override;
//
//	virtual Object *CreateObj();
//
//	static void StaticInit();
//
//	/*
//	Create a class with a name in a namespace.
//	\param name class name
//	If the name in the name space has already existed, assertion failure would
//	trigger.
//	\param pSuper The super class of this one.
//	\param pParent The namespace in which this class locates.
//	If the namespace is not specified, the parent is set to the global
//	namespace.
//	*/
//	static Class *Create(const IdString &name, Class *pSuper = nullptr,
//		RnNamespace *pParent = nullptr);
//private:
//	explicit Class(const IdString &name, Class *pSuper, RnNamespace *pParent);
//
//	Class *m_pSuper;
//};
//
///*
//The type of a fixed length array.
//The name and the parent of an array are automatically set by the system, \see 
//\a Array(). The element type of an array can be anoter array type, thus to 
//construct a multi-dimension array.
//*/
//class Array: public FType
//{
//	typedef FType Super_;
//public:
//	DECL_FIELD_TRAITS(Array)
//		
//	/*
//	Construct an array type.
//	During the construction , the parent is set to the global namespace; the 
//	name is set to "A[I]", where "I" is the item type. For example, the array 
//	"int foo[10]" in a nlang script is named after "@array[int#10]" internally.
//
//	\param pItemType The element type.
//	The element type must be a valid data type. 
//	\param length The number of elements.
//	\note The length can be 0, \see Length(). The total length of an array must
//	not be greater than \a N_MAX_ARRAY_LENGTH.
//	*/
//	Array(FType *pItemType, uint32 u4Length); 
//
//	/*
//	Get the element type.
//	The element type of an array can be another array type, thus to construct a 
//	multi-dimension array.
//	*/
//	RnNamed *ItemType() const
//	{
//		return m_pItemType;
//	}
//
//	//Get the number of dimensions of the array.
//	uint32 Dimensions() const;
//
//	/*
//	Get the number of elements in this dimension of the array.
//	If the length is 0, it represents an undetermined length array(ULA). A ULA 
//	is often used as a formal parameter of a function.
//	*/
//	uint32 Length() const
//	{
//		return m_u4Length;
//	}
//
//	/*
//	Get the number of elements of all the dimensions of the array.
//	If the total length is 0, it represents an undetermined length array(ULA).
//	*/
//	uint32 TotalLength() const;
//private:
//	//Automatically create the name string of an array.
//	static IdString BuildName(FType *pItemType, uint32 u4Length);
//	FType *m_pItemType;
//	uint32 m_u4Length;
//};


} //namespace nlang
