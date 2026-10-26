/*---
    BuiltinPrelude.h — the reserved prelude package ("builtins"): the
    package name plus the embedded source symbol that the build
    generates from stdlib/builtins.n (tools/embed_source.py).
---*/
#pragma once

namespace nlang
{
//Reserved prelude package name. It doubles as the pseudo
//TranslationUnit path of the embedded copy: with an empty package root
//DeriveModulePath's stem fallback yields exactly "builtins", so the
//registry's duplicate-package rule enforces the reservation for free.
inline constexpr const char* kPreludePackageName = "builtins";

//Embedded prelude source text (pointer to const chars; plain pointer
//spelling so the generated definition keeps external linkage). Defined
//in the GENERATED translation unit builtins_embedded.cpp (edit
//stdlib/builtins.n, not that file).
extern const char* kBuiltinPreludeSource;
} //namespace nlang
