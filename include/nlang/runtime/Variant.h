#pragma once
#include "TypeDef.h"
#include <string>
#include <typeinfo>   // typeid (the Get<T> assert) + std::type_info (m_pCppType member)
#include "StackDump.h"

namespace nlang
{

union VariantData
{
	int32			m_Int;
	int64			m_Long;     // 0.7.5: long/ulong/double literal storage
	uint64			m_ULong;
	float			m_Float;
	double			m_Double;
	std::string*	m_String;
	void*			m_Memory;
};

class RnDataType;

/*
\note Do not inherit from this class, since it has no vtable.
*/
class NLANG_RUNTIME_API Variant
{
public:
	//Copy ctor.
	explicit Variant(Variant& v);

	//Construct by a specified type and a value.
	template<typename FIELD_T>
	Variant(FIELD_T& type, typename FIELD_T::CppType value) :
		m_pType(&type), m_pCppType(type.CppTypeIdPtr())
	{
		type.InitValue(&m_Data, &value);
	}

	Variant(RnDataType& type, void* pValue);

	//Copy construct an int32 value.
	explicit Variant(int32 v);

	//Copy construct a float value.
	explicit Variant(float v);
	//Copy construct a string value.
	explicit Variant(const std::string& v);

	//Move construct a string value.
	explicit Variant(std::string&& v);

	~Variant();

	//Copy assignment.
	Variant& operator=(const Variant& rhs);

	//Get a cast value. m_pCppType holds the stored value's C++ type_info
	//address (set in the ctor from m_pType); comparing it here avoids
	//dereferencing m_pType->CppTypeId() in this template body, which needs
	//RnDataType complete at the definition point. This header forward-declares
	//RnDataType to stay light, and MSVC tolerates the incomplete access in a
	//template body where GCC does not.
	template<typename T>
	T Get() const
	{
		NLANG_ASSERT(&typeid(T) == m_pCppType);
		return static_cast<T>(m_Data.m_Memory);
	}

	VariantData& Data()
	{
		return m_Data;
	}

	const VariantData& Data() const
	{
		return m_Data;
	}

	//Get the type.
	RnDataType* Type()
	{
		return m_pType;
	}

	const RnDataType* Type() const
	{
		return m_pType;
	}

	std::string ToString() const;
private:
	RnDataType* m_pType;
	const std::type_info* m_pCppType;   // m_pType's C++ type_info (see Get<T>)
	VariantData m_Data;
};

//Explicit specializations of Get<T> for the union scalar channels.
//Defined out of class (GCC rejects in-class explicit member template
//specialization as ill-formed; MSVC accepts it), and inline because this
//header is included by multiple TUs.
template<> inline int32 Variant::Get<int32>() const
{
	return m_Data.m_Int;
}
template<> inline float Variant::Get<float>() const
{
	return m_Data.m_Float;
}
//0.7.5: 8-byte scalar channels. Kind-exact reads only — an int32
//write touches the low 4 bytes and leaves the upper half stale.
template<> inline int64 Variant::Get<int64>() const
{
	return m_Data.m_Long;
}
template<> inline uint64 Variant::Get<uint64>() const
{
	return m_Data.m_ULong;
}
template<> inline double Variant::Get<double>() const
{
	return m_Data.m_Double;
}

} // namespace nlang