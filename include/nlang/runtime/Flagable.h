/*-----------------------------------------------------------------------------
	nlang/intf/Flagable.h
	This file define the macros and interfaces of bit flags.
-----------------------------------------------------------------------------*/
#pragma once

namespace nlang
{

//Assign to pointer and destroy and its old content.
//Do nothing if the source and the destination are at the same address.
template<typename T>
inline void OverwritePtr(T*& pDst, T* pSrc)
{
	if (pDst != pSrc)
	{
		delete pDst;
		pDst = pSrc;
	}
}

/*
Define some implementation methods for bits set.
\param ftype The type of the bits set.
\param fname The name of the bits set.

e.g.
BIT_SET_METHODS_IMPL(int32, Flags);
*/	
#define BIT_SET_METHODS_IMPL(ftype, fname)								\
	/*Do the given flags is contained by our flags? */					\
	bool Contain##fname(ftype bits) const             					\
	{																	\
		return (m_##fname & bits) == bits;								\
	}																	\
																		\
	/*Do the given flags intersect with our flags? */					\
	bool Intersect##fname(ftype bits) const								\
	{																	\
		return (m_##fname & bits) != 0;									\
	}																	\
																		\
	/*Get flags */														\
	ftype fname() const													\
	{																	\
		return m_##fname;												\
	}																	\
																		\
	/*Set flags. */														\
	void fname(ftype bits)												\
	{																	\
		m_##fname = bits;												\
	}																	\
																		\
	/*Add flags. */														\
	void Add##fname(ftype bits)											\
	{																	\
		m_##fname |= bits;												\
	}																	\
																		\
	/*Remove the specified flags. */									\
	void Remove##fname(ftype bits)										\
	{																	\
		m_##fname &= ~bits;												\
	}																	\
																		\
	/*Clear all flags. */												\
	void Clear##fname()													\
	{																	\
		m_##fname = 0;													\
	}																	


template <typename BITS_T>
class Flagable
{
public:
	typedef BITS_T FlagBitsType;
public:
	BIT_SET_METHODS_IMPL(BITS_T, Flags);
protected:
	//To prevent from explicit deletion.							
	~Flagable()
	{															
	}															
private:
	BITS_T m_Flags;
};

template <typename BITS_T>
class ScopedFlagResetter
{
public:
	typedef Flagable<BITS_T> FlagbleType;
public:
	ScopedFlagResetter(FlagbleType &flagable) :
		m_Flagable(flagable), m_SavedFlags(flagable.Flags())
	{
	}

	~ScopedFlagResetter()
	{
		m_Flagable.Flags(m_SavedFlags);
	}
private:
	FlagbleType &m_Flagable;
	BITS_T m_SavedFlags;
};

#define SCOPED_FLAG_RESETER(flagable) \
	ScopedFlagResetter<decltype((flagable).Flags())> flagResseter##__LINE__(flagable)

} //namespace nlang
