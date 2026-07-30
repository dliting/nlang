/*-----------------------------------------------------------------------------
nlang/intf/Utils.cpp
This file implements the utility classes and functions in the nlang runtime.
-----------------------------------------------------------------------------*/

#include "Utils.h"
#include <ostream>
#include <iomanip>

namespace nlang
{

void NodeDumpAccessor::Access(const Node &node)
{
	const size_t nIndent = IndentOf(node);
	std::string s;
	try {
		s = node.ToString();
	} catch (...) {
		s = "!error";
	}

	if (nIndent > 0)
		m_Stream << std::setw(nIndent) << ' ';
	for (size_t i = 0, n = s.length(); i < n; i++)
	{
		m_Stream << s[i];
		//Insert indents before a new line.
		if (nIndent > 0 && s[i] == '\n')
			m_Stream << std::setw(nIndent) << ' ';
	}
	m_Stream << '\n';
}

size_t NodeDumpAccessor::IndentOf(const Node &node) const
{
	size_t nIndent = 0;
	for (auto pNode = &node; pNode != &m_Root; )
	{
		if (!pNode)
			break;
		nIndent += 4;
		pNode = pNode->Parent();
	}
	return nIndent;
}

} //namespace nlang