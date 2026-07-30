/*-----------------------------------------------------------------------------
nlang/intf/Utils.h
This file define some utility classes and functions for the nlang runtime.
-----------------------------------------------------------------------------*/

#pragma once
#include "Node.h"

namespace nlang
{

/*
A node accessor implementing node dump.
It can print the content of a node to std::ostream.
*/
class NLANG_RUNTIME_API NodeDumpAccessor
{
public:
	explicit NodeDumpAccessor(std::ostream &os, Node &root) :
		m_Stream(os), m_Root(root)
	{
	}

	//Template methods invoked the by a node visitor.
	void Access(const Node&);

	size_t IndentOf(const Node &node) const;
private:
	std::ostream &m_Stream;
	Node &m_Root;
};

} //namespace nlang
