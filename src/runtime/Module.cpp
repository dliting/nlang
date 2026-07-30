/*-----------------------------------------------------------------------------
	nlang/impl/Module.cpp
	This file implements the information of basic types in N Language.
-----------------------------------------------------------------------------*/

#include "Module.h"
#include "Log.h"
#include <errno.h>
#include <regex>
#include <filesystem>
#include <fstream>

namespace nlang
{

namespace fs = std::filesystem;

//Non-recursively search a file in the given directory.
bool FindFile_(const fs::path &pathDir, // in this directory,
	const std::string &sFileName,		// search for this name,
	std::string &sFoundPath)			// placing path here if found
{
	using namespace fs;
	fs::path filePath = pathDir / sFileName;
	if (!fs::exists(filePath))
		return false;
	if (fs::is_directory(filePath))
		return false;
	sFoundPath = std::move(filePath.string());
	return true;
}

Module::Module(const IdString &name) : m_Name(name)
{
}

bool Module::IsValidName(const std::string &sName)
{
	static const std::regex s_Pattern(N_ALL_NAME_PATTERN);
	return std::regex_match(sName, s_Pattern);
}

ModuleManager::ModuleManager() :
	m_upLoadedMap(new NameMap()),
	m_upLoadPaths(new std::list<std::string>())
{
}

ModuleManager::~ModuleManager()
{
	for (auto pModule : *this)
		delete pModule;
	// m_upLoadedMap and m_upLoadPaths are now unique_ptr - auto-deleted
}

Module *ModuleManager::Create(const std::string &sModuleName)
{
	if (!Module::IsValidName(sModuleName))
	{
		LogError("Create module failed, invalid module name: %s.", 
			sModuleName.c_str());
		errno = EINVAL;
		return 0;
	}
	const IdString name(sModuleName);
	if (Find(name))
	{
		LogError("Create module failed, module %s already exists.", 
			sModuleName.c_str());
		errno = EEXIST;
		return 0;
	}
	Module *pModule = new Module(name);
	m_upLoadedMap->emplace(std::make_pair(name, pModule));
	return pModule;
}

Module *ModuleManager::Load(const std::string &sModuleName)
{
	auto it = m_upLoadedMap->find(sModuleName);
	if (it != m_upLoadedMap->end())
		return it->second;

	const std::string sFilePath = FindModuleFile(sModuleName);
	if (sFilePath.empty())
	{
		errno = ENOENT;
		LogError("Cannot find the file of the module \"%s\".", 
			sModuleName.c_str());
		return 0;
	}

	std::ifstream is(sFilePath);
	if (!is)
	{
		LogError("Failed to open the module file \"%s\".", sFilePath.c_str());
		return 0;
	}

	Module *pModule = LoadFrom(is);
	if (!pModule)
	{
		LogError("Failed to load the module \"%s\".", sModuleName.c_str());
		return 0;
	}
	
	return pModule;
}

void ModuleManager::LoadPath(const std::list<std::string> &paths)
{
	*m_upLoadPaths = paths;
}

Module *ModuleManager::LoadFrom(std::istream &is)
{
	//TODO
	LogDebug("NModuleManager::Load(std::istream &is) is not implemented.\n");
	return 0;
}

std::string ModuleManager::FindModuleFile(const std::string &sModuleName) const
{
	static const char *MODULE_FILE_EXT = ".nmod";
	//Search the module file dir-by-dir until we find one.
	std::string sFoundPath; //empty path
	for (const auto &sDir : *m_upLoadPaths)
	{
		if (!fs::exists(sDir)) 
			continue;
		fs::path pathDir(sDir);
		if (FindFile_(pathDir, sModuleName + MODULE_FILE_EXT, sFoundPath))
			break;
	}
	return sFoundPath; 
}


} //namespace nlang
