/*-----------------------------------------------------------------------------
	ncomp/intf/TranslationUnit.h
	This file define the interface of a translation unit in the nlang compiler.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"
#include "SnMisc.h"
#include <list>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

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

	//Take ownership of the imports list (names of imported modules).
	//Phase 9c cross-module infrastructure: parser collects `import "X";`
	//statements at the top of the file and passes them here.
	void SetImports(std::vector<std::string>* pImports)
	{
		m_upImports.reset(pImports);
	}

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

	//Get the imported module names (from `import "X";` statements).
	const std::vector<std::string>& Imports() const
	{
		return *m_upImports;
	}

	SnNamespace *Root() const
	{
		return m_pRoot;
	}

	void ClearRoot()
	{
		m_pRoot = nullptr;
	}

	/*
	Phase 13: the type alias table (translation-unit scope). Populated by
	the TU-level alias pre-pass from `using Name = Type;` directives before
	translation units are merged; the table maps an alias name to the
	aliased type expression. The expressions stay owned by their SnUsing
	nodes — the table holds non-owning pointers and is only read during
	the pre-pass (before any using node is destroyed).
	*/
	//@{
	void SetAlias(const std::string& sName, SnFieldExpr *pType)
	{
		m_aliasTable[sName] = pType;
	}

	SnFieldExpr *FindAlias(const std::string& sName) const
	{
		auto iFound = m_aliasTable.find(sName);
		return iFound == m_aliasTable.end() ? nullptr : iFound->second;
	}
	//@}

private:
	std::unique_ptr<UsingList> m_upUsings;
	std::unique_ptr<std::vector<std::string>> m_upImports;
	SnNamespace *m_pRoot;
	std::unordered_map<std::string, SnFieldExpr*> m_aliasTable;
	const std::string m_sFilePath;
};

} //namespace nlang
