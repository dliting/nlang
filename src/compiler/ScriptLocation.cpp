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
	auto pCopy = std::make_unique<ScriptLocation>(*this);
	//Snapshot the file name: the TU is owned by the ModuleBuilder and
	//dies with it, while cloned log items must stay readable after
	//collection. A source that already carries a snapshot (a clone of
	//a clone) keeps it -- no TU dereference on possibly stale pointers.
	if (m_pTransUnit && pCopy->m_sFileName.empty())
		pCopy->m_sFileName = m_pTransUnit->FilePath();
	return pCopy;
}

std::string ScriptLocation::ToString() const
{
	//Prefer the Clone-time snapshot: dereferencing the TU is valid
	//only while the owning ModuleBuilder is alive.
	std::string sFileName = m_sFileName;
	if (sFileName.empty() && m_pTransUnit)
		sFileName = m_pTransUnit->FilePath();
	if (!sFileName.empty())
	{
		std::stringstream ss;
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