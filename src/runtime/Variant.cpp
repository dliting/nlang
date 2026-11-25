#include "Variant.h"
#include "RnTypes.h"

namespace nlang
{

Variant::Variant(Variant& v) : m_pType(v.Type()), m_pCppType(v.m_pCppType)
{
	m_pType->CopyValue(&m_Data, &v.m_Data);
}

Variant::Variant(RnDataType& type, void* pValue) :
	m_pType(&type), m_pCppType(type.CppTypeIdPtr())
{
	m_Data.m_Memory = pValue;
}

Variant::Variant(int32 v): m_pType(RnInt32::Instance()), m_pCppType(&RnInt32::Instance()->CppTypeId())
{
	m_Data.m_Int = v;
}

Variant::Variant(float v): m_pType(RnFloat::Instance()), m_pCppType(&RnFloat::Instance()->CppTypeId())
{
	m_Data.m_Float = v;
}

Variant::Variant(const std::string& v):	m_pType(RnString::Instance()), m_pCppType(&RnString::Instance()->CppTypeId())
{
	m_Data.m_String = new std::string(v);
}

Variant::Variant(std::string&& v):	m_pType(RnString::Instance()), m_pCppType(&RnString::Instance()->CppTypeId())
{
	m_Data.m_String = new std::string(std::move(v));
}

Variant::~Variant()
{
	m_pType->DestroyValue(&m_Data);
}

Variant& Variant::operator=(const Variant& rhs)
{
	if (this == &rhs)
		return *this;
	rhs.Type()->CopyValue(&m_Data, &rhs.m_Data);
	m_pType = rhs.m_pType;
	m_pCppType = rhs.m_pCppType;
	return *this;
}

std::string Variant::ToString() const
{
	return m_pType ? m_pType->ValueToString(&m_Data) : "unknown";
}


} //namespace nlang