/*-----------------------------------------------------------------------------
	nlang/impl/RnTypes.cpp
	This file implements the data type fields in nlang.
-----------------------------------------------------------------------------*/

#include "RnTypes.h"
#include "RnMisc.h"
#include "RuntimeNodeVisitor.h"
#include "Runtime.h"
#include <cassert>
#include <sstream>
#include <cstdio>

namespace nlang
{

RnDataType::RnDataType(NodeKind kind, FieldAccessType at, 
	const IdString &name) :
	Super_(kind, at, name)
{
}

RnBuiltinDataType::RnBuiltinDataType(NodeKind kind, const char* szName, 
	const type_info& cppTypeId) :
	Super_(kind, FA_Public, szName), m_CppTypeId(cppTypeId)
{
}

bool RnBuiltinDataType::CanAddChild(const RuntimeNode *pChild) const
{
	return false;
}

RnBuiltinDataType *RnBuiltinDataType::InstanceOf(NodeKind k)
{
	switch (k)
	{
#define MACRO_IMPL(T) \
	case NK_##T: return Rn##T::Instance();
		BUILTIN_TYPE_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
	default:
		return nullptr;
	}
}

void RnBuiltinDataType::StaticInit()
{
//register the instances.
#define MACRO_IMPL(T) \
	Runtime::GlobalNamespace().Members().push_back(Rn##T::Instance());
	BUILTIN_TYPE_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
}

void RnBuiltinDataType::InitValue(void *pDst, const void *pSrc) const
{
	CopyValue(pDst, pSrc);
}

const char *RnBuiltinDataType::NameOf(NodeKind k)
{
	return TraitsOf(k).m_szDesc;
}

ImmutableNodeList *RnBuiltinDataType::ChildrenPtr() const
{
	return ImmutableNodeList::NullList();
}

void RnBuiltinDataType::DestroyValue(void *pValue) const
{
	//Do nothing.
}

void RnInt32::Accept(IRuntimeNodeVisitor &v)
{
	v.Visit(*this);
}

std::string RnInt32::ValueToString(const void *pValue) const
{
	char szBuf[12];
	assert(pValue);
	const CppType v = *static_cast<const CppType*>(pValue);
	sprintf(szBuf, "%d", v);
	//itoa(v, szBuf, 10);
	return szBuf;
}

void RnFloat::Accept(IRuntimeNodeVisitor &v)
{
	v.Visit(*this);
}

std::string RnFloat::ValueToString(const void *pValue) const
{
	char szBuf[32];
	assert(pValue);
	const CppType v = *static_cast<const CppType*>(pValue);
	sprintf(szBuf, "%g", v);
	return szBuf;
}

void RnString::Accept(IRuntimeNodeVisitor &v)
{
	v.Visit(*this);
}

std::string RnString::ValueToString(const void *pValue) const
{
	const CppType *p = static_cast<const CppType*>(pValue);
	assert(p && *p);
	return **p;
}

void RnString::InitValue(void *pDst, const void *pSrc) const
{
	const CppType	*ppSrc = static_cast<const CppType*>(pSrc);
	CppType			*ppDst = static_cast<CppType*>(pDst);
	assert(ppDst && ppSrc);
	*ppDst = *ppSrc;
}

void RnString::CopyValue(void *pDst, const void *pSrc) const
{
	const CppType	*ppSrc = static_cast<const CppType*>(pSrc);
	CppType			*ppDst = static_cast<CppType*>(pDst);
	assert(ppDst && ppSrc && *ppDst && *ppSrc);
	**ppDst = **ppSrc;
}

void RnString::DestroyValue(void *pValue) const
{
	CppType *ppStr = static_cast<CppType*>(pValue);
	assert(ppStr);
	delete *ppStr;
	*ppStr = nullptr;
}

void RnType::Accept(IRuntimeNodeVisitor &v)
{
	v.Visit(*this);
}

std::string RnType::ValueToString(const void *pValue) const
{
	assert(pValue);
	const CppType v = *static_cast<const CppType*>(pValue);
	return v->ToString();
}

//Class *RootClass()
//{
//	assert(false && "Not implemented.");
//	return 0;
//}
//
//Class::Class(const IdString &name, Class *pSuper /*= 0 */, 
//	RnNamespace *pParent /*= 0 */ ):
//	Super_(name, 0), 
//	RnMemberContainer<RnNamed>(this),
//	m_pSuper(pSuper ? m_pSuper : RootClass())
//{
//	assert(RnNamed::IsValidName(name) &&"Invalid class name.");
//	if (!pParent) 
//		pParent = &Runtime::GlobalNamespace();
//	assert(!pParent->Members().Find(name) &&
//		"The field with the same name already exists.");
//	pParent->Members().Add(this);
//}
//
//bool Class::CanAddChild(const RnNamed *pChild) const
//{
//	assert(pChild &&"Cannot add a null pointer as a class member.");
//	switch (pChild->Kind())
//	{
//	case NK_Struct:
//	case NK_Enum:
//	case NK_Property:
//	case NK_Function:
//		{
//			auto pType = static_cast<const RnNamed*>(pChild);
//			return !Members().FindConflicted(*pType);
//		}
//	default:
//		return false;
//	}
//}
//
//void Class::Accept(IRuntimeNodeVisitor &v)
//{
//	v.Visit(*this);
//	Members().Accept(v);
//}
//
//Object *Class::CreateObj()
//{
//	//void *pMem = AllocObjMem();
//	//if (!pMem)
//	//	return 0;
//
//	////Set the pointer to the vtable.
//	//VTable* *ppVTable = static_cast<VTable**>(pMem);
//	//*ppVTable = VTable();
//
//	////Initialize the non-virtual members of the object.
//	//if (!InitObjMembers(pMem))
//	//	return 0;
//
//	//return static_cast<Object*>(pMem);
//	return 0;
//}
//
//Class *Class::Create(const IdString &name, Class *pSuper /*= nullptr*/, 
//	RnNamespace *pParent /*= nullptr*/)
//{
//	CREATE_NODE_RET(Class, name, pSuper, pParent);
//}
//
//Array::Array(FType *pItemType, uint32 u4Length):
//	Super_(BuildName(pItemType, u4Length), Runtime::RootClass(), 
//		&Runtime::GlobalNamespace()),
//	m_pItemType(pItemType), m_u4Length(u4Length)
//{
//	assert(pItemType);
//	assert(TotalLength() <= N_MAX_ARRAY_LENGTH);
//}
//
//uint32 Array::Dimensions() const
//{
//	if (m_pItemType->Kind() == NK_Array)
//	{
//		Array *pSubArray = static_cast<Array*>(m_pItemType);
//		return pSubArray->Dimensions() + 1;
//	}
//	return 1;
//}
//
//uint32 Array::TotalLength() const
//{
//	assert(m_pItemType);
//	if (m_pItemType->Kind() == NK_Array)
//	{
//		Array *pSubArray = static_cast<Array*>(m_pItemType);
//		return m_u4Length  *pSubArray->TotalLength();
//	}
//	return m_u4Length;
//}
//
//IdString Array::BuildName(FType *pItemType, uint32 u4Length)
//{
//	std::stringstream ss;
//	ss << "@array" << 
//		'[' << pItemType->Name().Value() << '#' << u4Length << ']';
//	return ss.str();
//}

} //namespace nlang