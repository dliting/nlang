/*-----------------------------------------------------------------------------
nlang/intf/Iterators.h
This file define the interfaces of helper iterators for nlang.
-----------------------------------------------------------------------------*/
#pragma once

namespace nlang
{

//The iterator of mutable value in a map.
template<class MAP_T>
class MapValueIterator
{
public:
	typedef typename MAP_T::mapped_type value_type;
	typedef typename MAP_T::iterator InnerIterator;
public:
	explicit MapValueIterator(MAP_T& map, const InnerIterator& inner): 
		m_Map(map), m_Iter(inner)
	{
	}

	MapValueIterator& operator++()
	{
		++m_Iter;
		return *this;
	}

	MapValueIterator& operator++(int)
	{
		m_Iter++;
		return *this;
	}

	value_type& operator*()
	{
		return m_Iter->second;
	}

	bool operator==(const MapValueIterator& rhs) const
	{
		return m_Iter == rhs.m_Iter;
	}

	bool operator!=(const MapValueIterator& rhs) const
	{
		return !operator==(rhs);
	}
private:
	MAP_T& m_Map;
	InnerIterator m_Iter;
};

//The iterator of constant value in a map.
template<class MAP_T>
class ConstMapValueIterator
{
public:
	typedef typename MAP_T::mapped_type value_type;
	typedef typename MAP_T::const_iterator InnerIterator;
public:
	explicit ConstMapValueIterator(const MAP_T& map, 
		const InnerIterator& inner): 
		m_Map(map), m_Iter(inner)
	{
	}

	ConstMapValueIterator& operator++()
	{
		++m_Iter;
		return *this;
	}

	ConstMapValueIterator& operator++(int)
	{
		m_Iter++;
		return *this;
	}

	const value_type& operator*() const
	{
		return m_Iter->second;
	}

	bool operator==(const ConstMapValueIterator& rhs) const
	{
		return m_Iter == rhs.m_Iter;
	}

	bool operator!=(const ConstMapValueIterator& rhs) const
	{
		return !operator==(rhs);
	}
private:
	const MAP_T& m_Map;
	InnerIterator m_Iter;
};

template<class T>
class CastIteratorBase
{
public:
	typedef T			value_type;
	typedef T *			pointer;
	typedef const T *	const_pointer;
	typedef T &			reference;
	typedef const T &	const_reference;
protected:
	~CastIteratorBase() {}
};

//Type cast iterator.
template <class ITER_T, typename T>
class CastIterator : public CastIteratorBase<T>
{
public:
	typedef CastIterator	iterator;
	typedef ITER_T			source_iterator;
	typedef typename CastIteratorBase<T>::value_type value_type;
	typedef typename CastIteratorBase<T>::pointer pointer;
	typedef typename CastIteratorBase<T>::reference reference;
public:
	explicit CastIterator(const source_iterator &iter) : m_Iter(iter)
	{
	}

	iterator & operator++()
	{
		++m_Iter;
		return *this;
	}

	iterator & operator++(int)
	{
		m_Iter++;
		return *this;
	}

	reference operator*() const
	{
		return static_cast<reference>(*m_Iter);
	}

	pointer operator->() const
	{
		return static_cast<pointer>(m_Iter.operator->());
	}

	bool operator==(const iterator &rhs) const
	{
		return m_Iter == rhs.m_Iter;
	}

	bool operator!=(const iterator &rhs) const
	{
		return !operator==(rhs);
	}

	source_iterator &Source()
	{
		return m_Iter;
	}
private:
	source_iterator m_Iter;
};

//Constant type cast iterator.
template <class ITER_T, typename T>
class ConstCastIterator : public CastIteratorBase<T>
{
public:
	typedef ConstCastIterator<ITER_T, T>	const_iterator;
	typedef ITER_T							source_iterator;
	typedef typename CastIteratorBase<T>::value_type value_type;
	typedef typename CastIteratorBase<T>::const_pointer const_pointer;
	typedef typename CastIteratorBase<T>::const_reference const_reference;
public:
	explicit ConstCastIterator(const source_iterator &iter) : m_Iter(iter)
	{
	}

	const_iterator &operator++()
	{
		++m_Iter;
		return *this;
	}

	const_iterator &operator++(int)
	{
		m_Iter++;
		return *this;
	}

	const_reference operator*() const
	{
		return static_cast<const_reference>(*m_Iter);
	}

	const_pointer operator->() const
	{
		return static_cast<const_pointer>(m_Iter.operator->());
	}

	bool operator==(const const_iterator &rhs) const
	{
		return m_Iter == rhs.m_Iter;
	}

	bool operator!=(const const_iterator &rhs) const
	{
		return !operator==(rhs);
	}

	source_iterator &Source()
	{
		return m_Iter;
	}
private:
	source_iterator m_Iter;
};

} //namespace nlang