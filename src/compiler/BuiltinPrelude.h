/*---
    BuiltinPrelude.h — the reserved prelude packages ("lang",
    "containers"): the package names plus the embedded source symbols
    that the build generates from stdlib/lang.n and
    stdlib/containers.n (tools/embed_source.py).
---*/
#pragma once

#include <cstddef>
#include <string>

namespace nlang
{
//Reserved prelude packages: each name doubles as the pseudo
//TranslationUnit path of its embedded copy (empty package root →
//DeriveModulePath's stem fallback yields exactly the name, so the
//registry's duplicate-package rule reserves each name for free).
//inline const, not constexpr: the sources are plain pointer variables
//defined in the GENERATED translation units (edit stdlib/<name>.n,
//not those files), and pointers to them are no constant expressions.
struct PreludePackage
{
	const char* name;
	const char* source;
};

extern const char* kLangPreludeSource;            //stdlib/lang.n
extern const char* kContainersPreludeSource;      //stdlib/containers.n

inline const PreludePackage kPreludePackages[] = {
	{"lang", kLangPreludeSource},
	{"containers", kContainersPreludeSource},
};

inline constexpr std::size_t kPreludePackageCount =
	sizeof(kPreludePackages) / sizeof(kPreludePackages[0]);

//True when dottedName spells a reserved prelude package.
inline bool IsPreludePackageName(const std::string& dottedName)
{
	for (std::size_t i = 0; i < kPreludePackageCount; ++i)
		if (dottedName == kPreludePackages[i].name)
			return true;
	return false;
}
} //namespace nlang
