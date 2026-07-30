/*-----------------------------------------------------------------------------
ncomp/intf/ScriptLocation.h
This file define the implementation of a script location in the nlang compiler.
-----------------------------------------------------------------------------*/
#include "ScriptLocation.h"
#include "TranslationUnit.h"
#include <sstream>

namespace nlang
{

ScriptLocation::ScriptLocation() :
	m_nStartLine(0),
	m_nEndLine(0),
	m_nStartCol(0),
	m_nEndCol(0),
	m_pTransUnit(nullptr)
{
}

std::unique_ptr<ISourceLocation> ScriptLocation::Clone() const
{
	return std::make_unique<ScriptLocation>(*this);
}

std::string ScriptLocation::ToString() const
{
	if (m_pTransUnit)
	{
		std::stringstream ss;
		std::string sFileName = m_pTransUnit->FilePath();
		ss << sFileName <<
			'(' << "line " << m_nStartLine << ", char " << m_nStartCol << ')';
		return ss.str();
	}
	return std::string();
}

TranslationUnit* ScriptLocation::TransUnit() const
{
	return m_pTransUnit;
}

} //namespace nlang