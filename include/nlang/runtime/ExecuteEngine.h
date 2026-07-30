/*-----------------------------------------------------------------------------
	nlang/intf/ExecuteEngine.h
	This file define the interface of the nlang execute engine.
-----------------------------------------------------------------------------*/

#pragma once
#include "RnMisc.h"

namespace nlang
{

//A class to execute nlang functions.
class NLANG_RUNTIME_API ExecuteEngine
{
public:
	/*
	Execute the "main" nlang function.
	In general, The "main" function is the entry point of an nlang application.
	This function must be declared in the global namespace.
	\return false on failure and the error code will be set.
	*/
	bool ExecuteMain(int& Return, int argc = 0, const char* rgv[] = 0);

	/*
	Execute the codes of a static nlang function.
	\param f The function to be executed.
	\param ... Variable arguments represent the return value and the params of 
	the function \a f.
	If the function's return type is not "void", the first argument must be the 
	return value. The remain arguments are the params of the function passed
	from left to right. 
	\return false on failure and the error code will be set.
	*/
	bool ExecuteStatic(RnFunction& , ...);

	/*
	Execute the codes of a non-static nlang member function in a class.
	\param f The function to be executed.
	\param obj The instance of the function owner.
	\param ... Variable arguments represent the return value and the params of 
	the function \a f.
	If the function's return type is not "void", the first argument must be the 
	return value. The remain arguments are the params of the function passed
	from left to right. 
	\return false on failure and the error code will be set.
	*/
	//bool ExecuteNonstatic(RnFunction& , Object& bj, ...);
};

} //namespace nlang
