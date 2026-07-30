/*-----------------------------------------------------------------------------
	nlang/intf/Module.h
	This file define the interfaces of modules in nlang.
-----------------------------------------------------------------------------*/
#pragma once
#include "IdString.h"
#include "CommonIterators.h"
#include <map>
#include <list>
#include <iostream>
#include <memory>

namespace nlang
{
	
//The memory representation of a executable or a library in nlang.
class NLANG_RUNTIME_API Module
{
	friend class ModuleManager;
public:

	//Get the name.
	//@{ 
	const IdString &Name() const
	{
		return m_Name;
	}

	const IdString &Name()
	{
		return m_Name;
	}
	//@}

	//Is the given string a valid module name?
	static bool IsValidName(const std::string &sName);
private:
	explicit Module(const IdString &name);
	const IdString m_Name;
};

//The class of the global module container.
class NLANG_RUNTIME_API ModuleManager : private CopyDisabled
{
public:
	typedef std::map<IdString, Module*> NameMap;
	typedef MapValueIterator<NameMap> iterator;
	typedef ConstMapValueIterator<NameMap> const_iterator;
public:
	//The destructor.
	//Destroy all modules.
	~ModuleManager();

	//Search a loaded module by name.
	Module *Find(const IdString &name) const
	{
		auto iPair = m_upLoadedMap->find(name);
		return iPair == m_upLoadedMap->end() ? 0 : iPair->second;
	}

	//STL compatible methods.
	//{@
	iterator begin()
	{
		return iterator(*m_upLoadedMap, m_upLoadedMap->begin());
	}

	const_iterator cbegin() const
	{
		return const_iterator(*m_upLoadedMap, m_upLoadedMap->cbegin());
	}

	iterator end()
	{
		return iterator(*m_upLoadedMap, m_upLoadedMap->end());
	}

	const_iterator cend() const
	{
		return const_iterator(*m_upLoadedMap, m_upLoadedMap->cend());
	}
	//@}

	/*
	Create and register a module.
	\return null if failed and the errno would be set to one of the following:
		\li EEXIST The module with the same name already exists.
		\li EINVAL The module name is invalid.
	*/
	Module *Create(const std::string &sModuleName);

	/*
	Load a module from a input stream to memory structure.
	If the module depends on other modules which are not loaded, those 
	dependent ones will be loaded first recursively. All the modules being 
	loaded are in a transaction. That's to say, they would be overall unloaded  
	on failure.
	The module would be search in the load paths of nlang, \see LoadPath().
	\return null if success else the module loaded.
	*/
	Module *Load(const std::string &sModuleName);

	/*
	Unload a module.
	To unload a module successfully, the flowing preconditions must be met:
		1. The modules depend on the given one must be unload first. 
		2. The objects in this module should not be referenced by outside.
	\return true if the module was successfully unloaded, otherwise return 
	false the errno would be set.
	*/
	bool Unload(Module*);

	/*
	Get the search paths of module loading.
	While loading modules, these paths will be searched by their listing order.
	*/
	//@{
	const std::list<std::string> &LoadPath() const
	{
		return *m_upLoadPaths;
	}

	std::list<std::string> &LoadPath()
	{
		return *m_upLoadPaths;
	}
	//@}

	/*
	Set the search directories of module loading.
	\note 
	The current working directory of the this process is always searched, even 
	when it is not in this input path list.
	*/
	void LoadPath(const std::list<std::string> &dirs);

	//Get the global module container.
	static ModuleManager &Instance()
	{
		static ModuleManager s_Instance;
		return s_Instance;
	}
private:
	ModuleManager();
	//Load module from a input stream.
	Module *LoadFrom(std::istream &is);
	//Find module file by module name.
	std::string FindModuleFile(const std::string &sModuleName) const;
	std::unique_ptr<NameMap> m_upLoadedMap;
	std::unique_ptr<std::list<std::string>> m_upLoadPaths;
};

}
