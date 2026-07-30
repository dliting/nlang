#pragma once
#include "TypeDef.h"
#include "CommonIterators.h"
#include <list>
#include <algorithm>
#include <cassert>

namespace nlang
{

class Node;

struct BypassNodeFilter 
{
	bool operator()(const Node *) const
	{
		return true;
	}
};

struct NodeIteratorBase
{
	typedef Node					value_type;
	typedef value_type *			pointer;
	typedef const value_type *		const_pointer;
	typedef value_type &			reference;
	typedef const value_type &		const_reference;
	typedef std::list<value_type *>	inner_list;
	virtual ~NodeIteratorBase() = 0 {};
};

//The base class of node iterator.
class NodeIterator : public NodeIteratorBase
{
	friend class ImmutableNodeList;
public:
	typedef NodeIterator			iterator;
	typedef inner_list::iterator	inner_iterator;
public:
	explicit NodeIterator(inner_list& items, inner_iterator pos) :
		m_Items(items), m_Iter(pos)
	{
	}

	NodeIterator(const iterator &other) :
		m_Items(other.m_Items), m_Iter(other.m_Iter)
	{
	}

	NodeIterator &operator=(const iterator &rhs)
	{
		m_Items = rhs.m_Items;
		m_Iter = rhs.m_Iter;
		return *this;
	}
	
	bool operator==(const iterator& rhs) const
	{
		return m_Iter == rhs.m_Iter;
	}

	bool operator!=(const iterator& rhs) const
	{
		return !operator==(rhs);
	}

	reference operator*() const
	{
		assert(m_Iter != m_Items.end());
		return **m_Iter;
	}

	pointer operator->() const
	{
		return get();
	}

	iterator &operator++()
	{
		assert(m_Iter != m_Items.end());
		++m_Iter;
		return *this;
	}

	iterator &operator++(int)
	{
		assert(m_Iter != m_Items.end());
		m_Iter++;
		return *this;
	}

	pointer get() const
	{
		assert(m_Iter != m_Items.end());
		return *m_Iter;
	}
protected:
	inner_list &m_Items;
	inner_iterator m_Iter;
};

template <class NODE_T>
using NodeIteratorT = CastIterator<NodeIterator, NODE_T>;

//The base class of constant node iterator.
class ConstNodeIterator : public NodeIteratorBase
{
	friend class ImmutableNodeList;
public:
	typedef ConstNodeIterator			const_iterator;
	typedef inner_list::const_iterator	inner_iterator;
public:
	explicit ConstNodeIterator(const inner_list &items, inner_iterator pos) :
		m_pItems(&items), m_Iter(pos)
	{
	}

	ConstNodeIterator(const ConstNodeIterator &other) :
		m_pItems(other.m_pItems), m_Iter(other.m_Iter)
	{
	}

	const_iterator &operator=(const const_iterator &rhs)
	{
		m_pItems = rhs.m_pItems;
		m_Iter = rhs.m_Iter;
		return *this;
	}

	bool operator==(const const_iterator& rhs) const
	{
		return m_Iter == rhs.m_Iter;
	}

	bool operator!=(const const_iterator& rhs) const
	{
		return !operator==(rhs);
	}

	const_reference operator*() const
	{
		assert(m_Iter != m_pItems->end());
		return **m_Iter;
	}

	const_pointer operator->() const
	{
		return get();
	}

	const_iterator &operator++()
	{
		assert(m_Iter != m_pItems->end());
		++m_Iter;
		return *this;
	}

	const_iterator &operator++(int)
	{
		assert(m_Iter != m_pItems->end());
		m_Iter++;
		return *this;
	}

	const_pointer get() const
	{
		assert(m_Iter != m_pItems->end());
		return *m_Iter;
	}
protected:
	const inner_list *m_pItems;
	inner_iterator m_Iter;
};

template <class NODE_T>
using ConstNodeIteratorT = ConstCastIterator<ConstNodeIterator, NODE_T>;

//Typed node iterator with a filter.
template <class FILTER_T>
class FiteredNodeIterator : public NodeIterator
{
	typedef NodeIterator Super_;
public:
	typedef FiteredNodeIterator<FILTER_T> iterator;
public:
	explicit FiteredNodeIterator(inner_list& items, inner_iterator pos) :
		Super_(items, pos)
	{
		GotoFilerted();
	}

	FiteredNodeIterator(const NodeIterator &other) : Super_(other)
	{
		GotoFilerted();
	}

	iterator & operator++()
	{
		assert(m_Iter != m_Items.end());
		++m_Iter;
		GotoFilerted();
		return *this;
	}

	iterator &operator++(int)
	{
		assert(m_Iter != m_Items.end());
		m_Iter++;
		GotoFilerted();
		return *this;
	}

	//End of position.
	iterator nop()
	{
		return iterator(m_Items, m_Iter.end());
	}
private:
	//Goto the nearest filtered node.
	void GotoFilerted()
	{
		m_Iter = std::find_if(m_Iter, m_Items.end(), m_Filter);
	}

	FILTER_T m_Filter;
};

template <class NODE_T, class FILTER_T>
using FilteredNodeIteratorT = 
	CastIterator<FiteredNodeIterator<FILTER_T>, NODE_T>;

//Constant node iterator with a filter.
template <class FILTER_T>
class ConstFiteredNodeIterator : public ConstNodeIterator
{
	typedef ConstNodeIterator Super_;
public:
	typedef ConstFiteredNodeIterator<FILTER_T> const_iterator;
public:
	explicit ConstFiteredNodeIterator(const inner_list& items, 
		inner_iterator pos) :
		Super_(items, pos)
	{
		GotoFilerted();
	}

	ConstFiteredNodeIterator(const ConstNodeIterator &other) : 
		Super_(other)
	{
		GotoFilerted();
	}

	const_iterator &operator++()
	{
		assert(m_Iter != m_pItems->end());
		++m_Iter;
		GotoFilerted();
		return *this;
	}

	const_iterator &operator++(int)
	{
		assert(m_Iter != m_pItems->end());
		m_Iter++;
		GotoFilerted();
		return *this;
	}

	//End of position.
	const_iterator nop()
	{
		return const_iterator(*m_pItems, m_Iter.end());
	}
private:
	//Goto the nearest filtered node.
	void GotoFilerted()
	{
		m_Iter = std::find_if(m_Iter, m_pItems->end(), m_Filter);
	}

	FILTER_T m_Filter;
};

template <class NODE_T, class FILTER_T>
using ConstFilteredNodeIteratorT =
	ConstCastIterator<ConstFiteredNodeIterator<FILTER_T>, NODE_T>;

} //namespace nlang