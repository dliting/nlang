#pragma once
#include "Node.h"
#include <map>
#include <algorithm>
#include <memory>
namespace nlang
{

struct DefaultFieldFilter
{
	DefaultFieldFilter()
	{
	}

	bool operator()(const Node *pNode) const
	{
		assert(pNode);
		return pNode->IsField();
	}
};

/*
Proxy class provides child node list modification of a given parent node.
This class is often used as a base class of those ones who like to modify the
child node list.
*/
class ChildNodeListController
{
protected:
	//Prevent from explicitly deletion.
	~ChildNodeListController()
	{
	}

	static void AddChildTo(Node *pChild, Node& parent)
	{
		parent.AddChild(pChild);
	}

	static NodeIterator InsertChildInto(NodeIterator iPos, Node *pChild,
		Node& parent)
	{
		return parent.InsertChild(iPos, pChild);
	}


	static NodeIterator RemoveChildFrom(NodeIterator iPos, Node& parent)
	{
		return parent.RemoveChild(iPos);
	}
};

/*
A nodes list which has the ownership of all the items and client programs 
should not modified it directly. 
\see ChildNodeListController to modify a child node list.
*/
class NLANG_RUNTIME_API ImmutableNodeList : private CopyDisabled
{
	friend class Node;
	typedef NodeIterator iterator;
	typedef ConstNodeIterator const_iterator;
	typedef std::list<Node *> inner_list;
public:
	explicit ImmutableNodeList();

	~ImmutableNodeList();

	iterator begin()
	{
		return iterator(*m_upItems, m_upItems->begin());
	}

	iterator end()
	{
		return iterator(*m_upItems, m_upItems->end());
	}

	iterator find(Node* pNode)
	{
		assert(pNode);
		return iterator(*m_upItems, 
			std::find(m_upItems->begin(), m_upItems->end(), pNode));
	}

	const_iterator find(const Node* pNode) const
	{
		assert(pNode);
		return const_iterator(*m_upItems,
			std::find(m_upItems->begin(), m_upItems->end(), pNode));
	}

	const_iterator cbegin() const
	{
		return const_iterator(*m_upItems, m_upItems->begin());
	}

	const_iterator cend() const
	{
		return const_iterator(*m_upItems, m_upItems->cend());
	}

	bool empty() const
	{
		return m_upItems->empty();
	}

	size_t size() const
	{
		return m_upItems->size();
	}

	//static empty node list instance.
	static ImmutableNodeList *NullList()
	{
		return &s_Null;
	}
protected:
	/*
	Add a node to the end of this list.
	Precondition: pNode != nullptr.
	*/
	void push_back(Node *pNode)
	{
		assert(this != NullList());
		assert(pNode != nullptr);
		m_upItems->push_back(pNode);
	}

	/*
	Insert a node to to the list at the given position.
	Precondition: &iPos.m_Items == &m_Items && pNode != nullptr.
	The node will be added before the location pointed by iPos.
	\return The iterator pointing to the inserted node.
	*/
	iterator insert(iterator& iPos, Node *pNode)
	{
		assert(this != NullList());
		assert(&iPos.m_Items == m_upItems.get() && pNode != nullptr);
		auto iter = m_upItems->insert(iPos.m_Iter, pNode);
		return iterator(*m_upItems, iter);
	}

	/*
	Remove a node from this list.
	Precondition: &iPos.m_Items == &m_Items && *iPos != nullptr.
	\return The iterator following the removed node.
	\note Remove a child will not destroy it.
	*/
	iterator erase(iterator& iPos)
	{
		assert(this != NullList());
		assert(&iPos.m_Items == m_upItems.get() && iPos != end());
		auto iter = m_upItems->erase(iPos.m_Iter);
		return iterator(*m_upItems, iter);
	}
private:
	std::unique_ptr<inner_list> m_upItems;
	static ImmutableNodeList s_Null;
};

template <class NODE_T, class FILTER_T>
class TypedNodeListBase : public ChildNodeListController
{
public:
	typedef typename std::conditional <
		std::is_same<FILTER_T, BypassNodeFilter>::value,
		NodeIteratorT<NODE_T>,
		FilteredNodeIteratorT <NODE_T, FILTER_T>> ::type iterator;
	typedef typename std::conditional <
		std::is_same<FILTER_T, BypassNodeFilter>::value,
		ConstNodeIteratorT<NODE_T>,
		ConstFilteredNodeIteratorT <NODE_T, FILTER_T>> ::type const_iterator;
	typedef NODE_T NodeType;
public:
	explicit TypedNodeListBase(Node *pParent) : m_pParent(pParent)
	{
		assert(m_pParent);
	}

	iterator begin()
	{
		return iterator(m_pParent->Children().begin());
	}

	iterator end()
	{
		return iterator(m_pParent->Children().end());
	}

	const_iterator begin() const
	{
		return cbegin();
	}

	const_iterator end() const
	{
		return cend();
	}

	const_iterator cbegin() const
	{
		return const_iterator(m_pParent->Children().cbegin());
	}

	const_iterator cend() const
	{
		return const_iterator(m_pParent->Children().cend());
	}
protected:
	//Prevent from explicit deletion.
	~TypedNodeListBase() {}

	/*
	Push back a field and take the ownership of it.
	Precondition: pField != nullptr.
	*/
	void push_back(NodeType *pNode)
	{
		assert(pNode != nullptr);
		assert(FILTER_T()(pNode));
		AddChildTo(pNode, *m_pParent);
	}

	/*
	Add a field to and take the ownership of it.
	Precondition: pField != nullptr.
	The field will be added before the location pointed by iPos.
	\return The iterator pointing to the inserted field.
	*/
	iterator insert(iterator iPos, NodeType *pNode)
	{
		assert(pNode == nullptr);
		assert(FILTER_T()(pNode));
		auto iNode = InsertChildInto(iPos.NodeIterator(), pNode, *m_pParent);
		return iterator(iNode);
	}

	/*
	Remove a field and release the ownership of it.
	Precondition: *iPos != nullptr && (*iPos)->Parent() == Parent().
	\return The iterator following the removed node.
	\note Erase a field will not destroy it.
	*/
	iterator erase(iterator iPos)
	{
		assert(iPos != end() && iPos->Parent() == m_pParent);
		auto iNode = RemoveChildFrom(iPos.Source(), *m_pParent);
		return iterator(iNode);
	}

	Node *m_pParent;
};

/*
A modifiable typed child node list.
This class provide the interface for modification of the child node list.
*/
template <class NODE_T, class FILTER_T = BypassNodeFilter>
class MutableChildNodeList : public TypedNodeListBase<NODE_T, FILTER_T>
{
	typedef TypedNodeListBase<NODE_T, FILTER_T> Super_;
public:
	explicit MutableChildNodeList(Node *pParent) : Super_(pParent)
	{
	}

	/*
	Push back a field and take the ownership of it.
	Precondition: pField != nullptr.
	*/
	void push_back(NodeType *pNode)
	{
		Super_::push_back(pNode);
	}

	/*
	Add a field to and take the ownership of it.
	Precondition: pField != nullptr.
	The field will be added before the location pointed by iPos.
	\return The iterator pointing to the inserted field.
	*/
	iterator insert(iterator iPos, NodeType *pNode)
	{
		return Super_::push_back(iPos, pNode);
	}

	/*
	Remove a field and release the ownership of it.
	Precondition: *iPos != nullptr && (*iPos)->Parent() == Parent().
	\return The iterator following the removed node.
	\note Erase a field will not destroy it.
	*/
	iterator erase(iterator iPos)
	{
		return Super_::erase(iPos);
	}
};

/*
A typed child field list shared the nodes with a child node list.
This class provide the interface for modification of the child node list and 
name lookup of fields.
\param FILTER_T The filter used for iteration in the shared node list.
The filter is used when the filed list is a fraction of the child node list.
*/
template <class FIELD_T, class FILTER_T = DefaultFieldFilter>
class ChildFieldList : public TypedNodeListBase<FIELD_T, FILTER_T>
{
	typedef TypedNodeListBase<FIELD_T, FILTER_T> Super_;
public:
	typedef std::list<FIELD_T *> field_list;
	typedef std::list<const FIELD_T *> const_field_list;
	typedef typename FIELD_T::NameType NameType;
	typedef NodeType FieldType;
	typedef std::multimap<NameType, FieldType *> OrderedNameMap;
public:
	explicit ChildFieldList(Node *pParent) : Super_(pParent)
	{
	}

	//create from a STL list.
	ChildFieldList(field_list& fields, Node *pParent) : Super_(pParent)
	{
		for (auto pField : fields)
			push_back(pField);
	}

	/*
	Push back a field and take the ownership of it.
	Precondition: pField != nullptr.
	The container permits same-name entries (function overloads, or
	duplicate fields pending conflict detection). DuplicateFieldChecker
	is the single source of truth for conflict reporting — it runs after
	the full AST is assembled and produces clean diagnostics, so the
	container must not abort early with an assertion.
	*/
	void push_back(FieldType *pField)
	{
		assert(pField);
		Super_::push_back(pField);
		m_NameMap.emplace(pField->Name(), pField);
	}

	/*
	Add a field and take the ownership of it.
	Precondition: pField != nullptr.
	The field will be added before the location pointed by iPos.
	\return The iterator pointing to the inserted field.
	*/
	iterator insert(iterator iPos, FieldType *pField)
	{
		assert(pField);
		auto iNode = Super_::insert(iPos, pField);
		m_NameMap.emplace(pField->Name(), pField);
		return iNode;
	}

	/*
	Remove a field and release the ownership of it.
	Precondition: *iPos != nullptr && (*iPos)->Parent() == Parent().
	\return The iterator following the removed node.
	\note Erase a field will not destroy it.
	*/
	iterator erase(iterator iPos)
	{
		auto& node = *iPos;
		auto iNode = Super_::erase(iPos);
		//Remove only this specific entry from the multimap. Functions may
		//be overloaded (same name, multiple entries), so erase-by-key
		//would corrupt the dictionary by removing sibling overloads.
		auto range = m_NameMap.equal_range(node.Name());
		for (auto it = range.first; it != range.second; ++it) {
			if (it->second == &node) {
				m_NameMap.erase(it);
				break;
			}
		}
		return iNode;
	}

	size_t size() const
	{
		return m_NameMap.size();
	}

	/*
	Find a field by name.
	\note There may be more than one field match the name, but we only return
	one of them on found.
	//@{
	*/
	FieldType *find(const NameType &name)
	{
		auto iFound = m_NameMap.find(name);
		return iFound == m_NameMap.end() ? nullptr : iFound->second;
	}

	const FieldType *find(const NameType &name) const
	{
		auto iFound = m_NameMap.find(name);
		return iFound == m_NameMap.end() ? nullptr : iFound->second;
	}
	//@}

	////Find all fields match the specified name.
	////@{
	//field_list find_all(const NameType &name)
	//{
	//	field_list results;
	//	auto r = m_NameMap.equal_range(&name);
	//	for (auto iter = r.first; iter != r.second; iter++)
	//		results.push(iter.second);
	//	return std::move(results);
	//}

	//const_field_list find_all(const NameType &name) const
	//{
	//	const_field_list results;
	//	auto r = m_NameMap.equal_range(&name);
	//	for (auto iter = r.first; iter != r.second; iter++)
	//		results.push(iter.second);
	//	return std::move(results);
	//}
	////@}

	const FieldType *FindConflicted(const FieldType &other) const
	{
		auto r = m_NameMap.equal_range(other.Name());
		for (auto iter = r.first; iter != r.second; iter++)
		{
			auto pExisted = iter->second;
			if (pExisted->ConflictedWith(other))
				return pExisted;
		}
		return nullptr;
	}

	const OrderedNameMap& NameDict() const
	{
		return m_NameMap;
	}
private:
	OrderedNameMap m_NameMap;
};

} //namespace nlang