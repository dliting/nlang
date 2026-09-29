#pragma once
#include "TypeDef.h"
#include <string>

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
	//0.7.5: 8-byte scalar channels. Kind-exact reads only — an int32
	//write touches the low 4 bytes and leaves the upper half stale.
	template<>
	int64 Get<int64>() const
	{
		return m_Data.m_Long;
	}
	template<>
	uint64 Get<uint64>() const
	{
		return m_Data.m_ULong;
	}
	template<>
	double Get<double>() const
	{
		return m_Data.m_Double;
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