/*-----------------------------------------------------------------------------
	ncomp/intf/TranslationUnit.h
	This file define the implementation of a translation unit in the nlang 
compiler.
-----------------------------------------------------------------------------*/

#include "TranslationUnit.h"
#include "SnExtraTypes.h"

namespace nlang
{

TranslationUnit::TranslationUnit(const std::string& sFilePath):
	m_sFilePath(sFilePath), m_pRoot(nullptr), m_upUsings(nullptr)
{
}

TranslationUnit::~TranslationUnit()
{
	delete m_pRoot;
	// m_upUsings is now unique_ptr - auto-deleted
}

void TranslationUnit::Init(PtrList<SnUsing>* pUsings,
	PtrList<SnField>* pFields, const ISourceLocation &loc)
{
	assert(!m_pRoot && !m_upUsings);
	m_upUsings = std::make_unique<UsingList>(pUsings);
	auto pName = new std::string(GLOBAL_NAMESPACE_NAME);
	m_pRoot = new SnNamespace(pName, pFields, loc);
}

} //namespace nlang
