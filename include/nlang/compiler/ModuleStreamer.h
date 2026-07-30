/*-----------------------------------------------------------------------------
	ncomp/intf/ModuleStreamer.h
	This file define the interface of the module serialization.
-----------------------------------------------------------------------------*/
#pragma once
#include <iosfwd>
#include "Config.h"

namespace nlang
{

//The base class of module I/O facility.
class NLANG_COMPILER_API NModuleStreamer
{

};

//The class of module loading facility.
class NLANG_COMPILER_API NModuleLoader: public NModuleStreamer
{
public:
	/*
	Load a module from a input stream to memory structure.
	If the module depends on other modules which are not loaded, those
	dependent ones will be loaded first recursively. All the modules being
	loaded are in a transaction. That's to say, they would be overall unloaded
	on failure.
	\return null if success else the module loaded.
	*/
	Module* Load(std::istream&);
};

//The class of module saving facility.
class NLANG_COMPILER_API NModuleSaver: public NModuleStreamer
{

};

} //namespace nlang
