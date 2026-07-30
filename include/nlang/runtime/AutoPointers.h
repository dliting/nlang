/*-----------------------------------------------------------------------------
	ncomp/intf/AutoPointers.h
	This file define the containers of auto delete pointers.
-----------------------------------------------------------------------------*/

#pragma once
#include <list>
#include <vector>
#include <functional>

namespace nlang
{

template <typename T>
struct DefaultDeleter
{
	void operator()(T* pItem)
	{
		delete pItem;
	}
};

//A unique pointer without thread safe guarantee.
template <typename T>
struct UniquePtr
{
public:
	typedef typename std::function<void(T*)> Deleter;
public:
	UniquePtr() : 
		m_pValue(nullptr), m_Deleter(DefaultDeleter<T>())
	{
	}

	UniquePtr(T* p, const Deleter& d = DefaultDeleter<T>()) :
		m_pValue(p), m_Deleter(d)
	{
	}

	//Copy constructor.
	UniquePtr(UniquePtr<T>& rhs, const Deleter& d = DefaultDeleter<T>()) :
		m_pValue(rhs.m_pValue), m_Deleter(d)
	{
		rhs.m_pValue = nullptr;
	}

	~UniquePtr()
	{
		m_Deleter(m_pValue);
	}

	UniquePtr<T>& operator=(UniquePtr<T>& rhs)
	{
		if (&rhs == this)
			return *this;
		Reset(rhs.m_pValue);
		rhs.m_pValue = nullptr;
		return *this;
	}

	UniquePtr<T>& operator=(T* pValue)
	{
		if (m_pValue == pValue)
			return *this;
		Reset(pValue);
		return *this;
	}

	T* operator->()
	{
		return m_pValue;
	}

	const T* operator->() const
	{
		return m_pValue;
	}

	T& operator*()
	{
		assert(m_pValue);
		return *m_pValue;
	}

	const T& operator*() const
	{
		assert(m_pValue);
		return *m_pValue;
	}

	bool operator!() const
	{
		return !m_pValue;
	}

	operator bool() const
	{
		return m_pValue != nullptr;
	}

	void Reset(T* pValue, const Deleter& d)
	{
		Reset(pValue);
		m_Deleter = d;
	}

	void Reset(T* pValue)
	{
		assert(!pValue || m_pValue != pValue);
		m_Deleter(m_pValue);
		m_pValue = pValue;
	}

	T* Get() const
	{
		return m_pValue;
	}
private:
	T* m_pValue;
	Deleter m_Deleter;
};

//Generic collection of auto delete pointers.
template <typename T, class COLLECTION_T>
class UniquePtrCollection
{
public:	
	typedef UniquePtrCollection<T, COLLECTION_T>	collection;
	typedef typename COLLECTION_T					inner_collection;
	typedef typename std::function<void(T*)>		item_deleter;
	typedef typename COLLECTION_T::iterator			iterator;
	typedef typename COLLECTION_T::const_iterator	const_iterator;
public:
	explicit UniquePtrCollection(const item_deleter& deleter) : 
		m_ItemDeleter(deleter)
	{
	}

	UniquePtrCollection(inner_collection *pItems, const item_deleter& deleter) :
		m_ItemDeleter(deleter)
	{
		if (pItems)
		{
			m_Items = std::move(*pItems);
			delete pItems;
		}
	}

	//Move constructor.
	UniquePtrCollection(collection &&other)
	{
		MoveFrom(other);
	}

	//Declared as protected method to prevent explicit deletion.
	virtual ~UniquePtrCollection()
	{
		DeleteAll();
	}

	//Move assignment.
	collection & operator=(collection &&other)
	{
		MoveFrom(other);
		return *this;
	}

	//\name STL like functions.
	///@{
	iterator begin()
	{
		return m_Items.begin();
	}

	iterator end()
	{
		return m_Items.end();
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
		return m_Items.cbegin();
	}

	const_iterator cend() const
	{
		return m_Items.cend();
	}

	bool empty() const
	{
		return m_Items.empty();
	}

	size_t size() const
	{
		return m_Items.size();
	}

	//Destroy all items and clear the collection.
	void clear() 
	{
		DoClear();
	}

	//Add an item and take the ownership of it.
	void Add(T* pItem)
	{
		DoAdd(pItem);
	}
	///@}
	
	//Release the ownership of all items and clear the collection.
	void Release()
	{
		m_Items.clear();
	}
protected:
	virtual void DoAdd(T* pItem) = 0;

	virtual void DoClear()
	{
		DeleteAll();
		m_Items.clear();
	}

	void MoveFrom(collection &rhs)
	{
		if (&rhs != this)
			m_Items = std::move(rhs.m_Items);
	}

	inner_collection m_Items;
private:
	void DeleteAll()
	{
		if (m_ItemDeleter)
		{
			for (auto pItem : m_Items)
				m_ItemDeleter(pItem);
		}
	}

	item_deleter m_ItemDeleter;
};

//Auto delete pointer list.
template<typename T>
class UniquePtrList : public UniquePtrCollection<T, std::list<T*>>
{
	typedef UniquePtrCollection<T, std::list<T*>> Super_;
public:
	typedef UniquePtrList<T> pointer_list;
	using typename Super_::item_deleter;
	using typename Super_::inner_collection;
public:
	explicit UniquePtrList(const item_deleter& deleter = DefaultDeleter<T>()) : 
		Super_(deleter)
	{
	}

	//Move constructor.
	UniquePtrList(pointer_list &&rhs)
	{
		Super_::MoveFrom(rhs);
	}

	//Construct from a STL list pointer and delete it.
	UniquePtrList(inner_collection *pItems,
		const item_deleter& deleter = DefaultDeleter<T>()) :
		Super_(pItems, deleter)
	{
	}

	pointer_list& operator=(pointer_list &&rhs)
	{
		Super_::MoveFrom(rhs);
		return *this;
	}
protected:
	void DoAdd(T* pItem) override
	{
		m_Items.push_back(pItem);
	}
};

//Auto delete pointer vector.
template<typename T>
class AutoPtrVector : public UniquePtrCollection<T, std::vector<T*>>
{
	typedef UniquePtrCollection<T, std::vector<T*>> Super_;
public:
	typedef AutoPtrVector<T> pointer_vector;
	using typename Super_::item_deleter;
	using typename Super_::inner_collection;
public:
	explicit AutoPtrVector(const item_deleter& deleter = DefaultDeleter<T>()) :
		Super_(deleter)
	{
	}

	AutoPtrVector(inner_collection *pVector,
		const item_deleter& deleter = DefaultDeleter<T>()) :
		Super_(pVector, deleter)
	{
	}

	//Move constructor.
	AutoPtrVector(pointer_vector &&rhs)
	{
		Super_::MoveFrom(rhs);
	}

	//Move assignment.
	pointer_vector & operator=(pointer_vector &&rhs)
	{
		Super_::MoveFrom(rhs);
		return *this;
	}

	T* operator[](size_t index)
	{
		return m_Items[index];
	}

	const T* operator[](size_t index) const
	{
		return m_Items[index];
	}
protected:
	void DoAdd(T* pItem) override
	{
		m_Items.push_back(pItem);
	}
};

} //namespace nlang