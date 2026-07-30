#pragma once
#include "TypeDef.h"
#include <string>

namespace nlang
{

union VariantData
{
	int32			m_Int;
	float			m_Float;
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
		m_pType(&type)
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

	//Get a cast value.
	template<typename T>
	T Get() const
	{
		assert(&typeid(T) == m_pType->CppTypeId());
		return static_cast<T>(m_Data.m_Memory);
	}

	template<>
	int32 Get<int32>() const
	{
		return m_Data.m_Int;
	}
		template<>
		float Get<float>() const
		{
			return m_Data.m_Float;
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
	VariantData m_Data;
};

} // namespace nlang