#pragma once
#include "BuildEnvironment.h"
#include <nlang/runtime/Module.h>
#include <filesystem>
#include <fstream>

namespace nlang
{

//Module file header.
struct ModuleHeader
{
	uint8				m_pcMagicNumber[8];
	uint16				m_u2MajorVersion;
	uint16				m_u2MinorVersion;
	uint8				m_pcCheckSum[16];
	char				m_szModuleName[256 + 1];
	uint32				m_u4Size;
};

//The utility class to save the current compiled module.
class CompiledModuleSaver
{
public:
	explicit CompiledModuleSaver(BuildEnvironment &env) : m_Env(env)
	{
	}

	bool Execute()
	{
		std::string sFilePath;
		if (!GetModulePath(sFilePath))
			return false;

		m_Env.Log(CLL_Info, "Output module file: %s ...", sFilePath.c_str());
		std::fstream fs;
		fs.open(sFilePath, std::ios::binary | std::ios::out);
		if (!fs.is_open())
		{
			m_Env.Log(CLL_Fatal, "Failed to open the file: %s.",
				sFilePath.c_str());
			return false;
		}

		//TODO: implement module serialization
		fs.close();
		return true;
	}
private:
	bool GetModulePath(std::string &sFilePath)
	{
		static const char *MODULE_FILE_EXT = "nmod";
		namespace bf = std::filesystem;
		const BuildParams &setting = m_Env.Params();
		const std::string sFileName =
			setting.m_sOutputModule + "." + MODULE_FILE_EXT;
		if (setting.m_sOutputDir.empty())
		{
			bf::path modulePath = bf::current_path() / sFileName;
			sFilePath = modulePath.string();
			return true;
		}

		bf::path modulePath(setting.m_sOutputDir);
		if (!bf::exists(modulePath))
		{
			try
			{
				bf::create_directory(modulePath);
			}
			catch (std::exception &e)
			{
				m_Env.Log(CLL_Fatal, "Failed to create output directory %s: %s.",
					modulePath.string().c_str(), e.what());
				return false;
			}
		}
		else if (!bf::is_directory(modulePath))
		{
			m_Env.Log(CLL_Fatal, "Invalid output directory: %s.",
				modulePath.string().c_str());
			return false;
		}

		modulePath /= sFileName;
		sFilePath = modulePath.string();
		return true;
	}

	BuildEnvironment &m_Env;
};

} //namespace nlang
