/*-----------------------------------------------------------------------------
	ncomp/intf/TypeDef.h
	This file define the common types and constants in the nlang compiler.
-----------------------------------------------------------------------------*/

#pragma once
#include "Config.h"
#include <nlang/runtime/TypeDef.h>
#include <string>
#include <memory>

namespace nlang
{

//The log levels in compiling.
enum CompileLogLevel
{
	CLL_Info,
	CLL_Warn,
	CLL_Error,
	CLL_Fatal,
	CLL_More,	//more log information following the last log item.
	CLL_COUNT
};

class TranslationUnit;

/*
The interface to represent a location in source codes.
It takes into account the location representation both in script codes and in
chart ones.
*/
struct ISourceLocation
{
	virtual ~ISourceLocation() = default;

	//create a copy of this location.
	virtual std::unique_ptr<ISourceLocation> Clone() const = 0;

	virtual std::string ToString() const = 0;

	//Get the translation unit of this source location.
	virtual TranslationUnit* TransUnit() const = 0;
};

} //namespace nlang
