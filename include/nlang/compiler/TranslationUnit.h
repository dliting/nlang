/*-----------------------------------------------------------------------------
	ncomp/intf/TranslationUnit.h
	This file define the interface of a translation unit in the nlang compiler.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"
#include "SnMisc.h"
#include <list>
#include <memory>

namespace nlang
{

class SnNamespace;
class CompileLogger;

//A translation unit of an nlang source file.
class NLANG_COMPILER_API TranslationUnit
{
public:
	typedef UniquePtrList<SnField> FieldList;
public:
	//Construct a translation unit with a file path.
	explicit TranslationUnit(const std::string& FilePath);

	~TranslationUnit();

	//Initialize the namespaces using list and the field list.
	void Init(PtrList<SnUsing>* pUsings, PtrList<SnField>* pFields,
		const ISourceLocation &loc);

	//Get the source file path of this translation unit.
	const std::string& FilePath() const
	{
		return m_sFilePath;
	}

	//Get the namespaces using list.
	UsingList *Usings() const
	{
		return m_upUsings.get();
	}

	SnNamespace *Root() const
	{
		return m_pRoot;
	}

	void ClearRoot()
	{
		m_pRoot = nullptr;
	}

private:
	std::unique_ptr<UsingList> m_upUsings;
	SnNamespace *m_pRoot;
	const std::string m_sFilePath;
};

} //namespace nlang
