/*-----------------------------------------------------------------------------
	compiler/Utf8.h
	The shared UTF-8 input contract for source and project files: strict
	well-formedness validation plus BOM handling. Both the scanner (`.n`)
	and the project loader (`.nproj`) funnel their raw bytes through here,
	so an encoding mistake is rejected with one named diagnosis instead of
	smuggling mojibake into tokens.
-----------------------------------------------------------------------------*/

#pragma once
#include <cstddef>
#include <string>

#include "TypeDef.h"

namespace nlang
{

//True when `bytes` is well-formed UTF-8: valid 1-4 byte sequences only.
//Overlong encodings, UTF-16 surrogates, code points above U+10FFFF and
//stray or truncated continuation bytes all fail.
NLANG_COMPILER_API bool Utf8Valid(const char* bytes, size_t length);

//Check the input contract shared by `.n` and `.nproj` files: an optional
//UTF-8 BOM (accepted and skipped) followed by valid UTF-8.
//On success returns true with *content/*contentLength pointing at the
//text after the BOM. On failure returns false and fills *reason with a
//diagnosis that names the condition but not the file (the caller knows
//the noun and the path): "is not valid UTF-8 (first invalid byte at
//line N)" or "is UTF-16, not UTF-8".
NLANG_COMPILER_API bool Utf8ContentCheck(const char* bytes, size_t length,
	const char** content, size_t* contentLength, std::string* reason);

} //namespace nlang
