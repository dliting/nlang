/*---
    NcuPackageCache.cpp — the extraction cache's whole policy in one
    translation unit: root resolution, directory key, landing check,
    atomic publish (see the header for the contract).
---*/
#include "NcuPackageCache.h"
#include "nlang/core/NcuPackage.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nlang
{
namespace
{

namespace fs = std::filesystem;

//The cache root: NLANG_PKG_CACHE_ROOT overrides (the NLANG_PATH
//precedent — tests and sandboxes redirect the cache), otherwise the
//standard per-user cache location. Empty return = no root resolvable;
//the caller treats a package that needs the cache as unusable.
std::string PackageCacheRoot()
{
	if (const char* env = std::getenv("NLANG_PKG_CACHE_ROOT"))
		if (*env)
			return env;
#if defined(_WIN32)
	if (const char* localAppData = std::getenv("LOCALAPPDATA"))
		if (*localAppData)
			return std::string(localAppData) + "\\nlang\\pkg-cache";
#else
	if (const char* xdg = std::getenv("XDG_CACHE_HOME"))
		if (*xdg)
			return std::string(xdg) + "/nlang/pkg-cache";
	if (const char* home = std::getenv("HOME"))
		if (*home)
			return std::string(home) + "/.cache/nlang/pkg-cache";
#endif
	return std::string();
}

//Stable directory key: FNV-1a 64 of the absolute package path in
//hex — std::hash is not stable across runs, NcuChecksum is (one
//well-known primitive, already public in NcuPackage.h).
std::string PackageCacheDirName(const std::string& packagePath)
{
	std::error_code ec;
	const std::string absolute =
		fs::absolute(fs::path(packagePath), ec).string();
	const uint64_t hash = NcuChecksum(absolute.data(), absolute.size());
	static const char* kHexDigits = "0123456789abcdef";
	std::string name;
	name.reserve(16);
	for (int shift = 60; shift >= 0; shift -= 4)
		name.push_back(kHexDigits[(hash >> shift) & 0xF]);
	return name;
}

bool ReadFileBytes(const fs::path& file, std::string& bytes)
{
	std::ifstream in(file, std::ios::binary);
	if (!in)
		return false;
	bytes.assign(std::istreambuf_iterator<char>(in),
		std::istreambuf_iterator<char>());
	return true;
}

//One native member onto the disk: publish through a process-unique
//temporary name and an atomic rename, so a concurrent reader at worst
//sees the old complete file. std::filesystem::rename replaces an
//existing target on Windows (POSIX semantics, MSVC STL).
bool PublishCacheFile(const fs::path& target, const std::string& bytes,
	std::string* error)
{
	fs::path temp = target;
	temp += ".tmp-";
#if defined(_WIN32)
	temp += std::to_string(GetCurrentProcessId());
#else
	temp += std::to_string(static_cast<long>(getpid()));
#endif
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		if (!out)
		{
			std::error_code removeError;
			fs::remove(temp, removeError);
			if (error)
				*error = "cannot write package cache file '"
					+ temp.string() + "'";
			return false;
		}
	}
	std::error_code renameError;
	fs::rename(temp, target, renameError);
	if (renameError)
	{
		std::error_code removeError;
		fs::remove(temp, removeError);
		if (error)
			*error = "cannot publish package cache file '"
				+ target.string() + "': " + renameError.message();
		return false;
	}
	return true;
}

//The package's cache directory (root + stable hash key), created on
//demand. False with *error when no root resolves or the directory
//cannot be made; the caller treats a package that needs the cache as
//unusable.
bool EnsurePackageCacheDir(const std::string& packagePath,
	fs::path& cacheDirOut, std::string* error)
{
	const std::string root = PackageCacheRoot();
	if (root.empty())
	{
		if (error)
			*error = "package '" + packagePath + "' carries native "
				"member(s) but no package cache root could be resolved "
				"(set NLANG_PKG_CACHE_ROOT)";
		return false;
	}
	cacheDirOut = fs::path(root) / PackageCacheDirName(packagePath);
	std::error_code ec;
	fs::create_directories(cacheDirOut, ec);
	if (ec)
	{
		if (error)
			*error = "cannot create package cache directory '"
				+ cacheDirOut.string() + "': " + ec.message();
		return false;
	}
	return true;
}

//Every native member of pkg into cacheDir: extract (the reader verifies
//the member checksum), landing-check against the file on disk, publish
//the mismatch. False with *error on the first failure.
bool WriteNativeMembers(const NcuPackageReader& pkg, const fs::path& cacheDir,
	std::string* error)
{
	const auto& memberPaths = pkg.MemberPaths();
	const auto& memberKinds = pkg.MemberKinds();
	for (size_t i = 0; i < memberPaths.size(); ++i)
	{
		if (memberKinds[i] != NcuMemberKind::Native)
			continue;
		std::string bytes;
		std::string extractError;
		//ExtractMember verifies the member checksum against the table.
		if (!pkg.ExtractMember(memberPaths[i], &bytes, &extractError))
		{
			if (error)
				*error = extractError;
			return false;
		}
		const fs::path target =
			cacheDir / fs::path(memberPaths[i]).filename();
		//Landing check: byte equality with the member subsumes the
		//checksum comparison and needs no new reader accessor.
		std::string onDisk;
		if (ReadFileBytes(target, onDisk) && onDisk == bytes)
			continue;
		if (!PublishCacheFile(target, bytes, error))
			return false;
	}
	return true;
}

} //namespace

void ExtractNativeMembersToCache(const NcuPackageReader& pkg,
	const std::string& packagePath, std::vector<std::string>& cacheDirs,
	std::string* error)
{
	const auto& memberKinds = pkg.MemberKinds();
	const bool carriesNative = std::any_of(
		memberKinds.begin(), memberKinds.end(),
		[](const NcuMemberKind kind)
		{ return kind == NcuMemberKind::Native; });
	if (!carriesNative)
		return;
	fs::path cacheDir;
	if (!EnsurePackageCacheDir(packagePath, cacheDir, error))
		return;
	if (!WriteNativeMembers(pkg, cacheDir, error))
		return;
	cacheDirs.push_back(cacheDir.string());
}

} //namespace nlang
