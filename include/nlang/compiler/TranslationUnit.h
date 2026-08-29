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

//One `import` statement: a dotted module path with an optional
//trailing `*` wildcard. The wildcard is a PREFIX match in module-path
//space — `import utils.*;` reaches utils/ and every nested
//subdirectory (utils.helper, utils.sub.x, ...).
struct NLANG_COMPILER_API ImportSpec
{
	//Path segments in declaration order; the '*' is not stored.
	std::vector<std::string> segments;
	//True when the last segment was '*'.
	bool wildcard = false;

	//"utils.helper" for {utils, helper}; wildcard excluded.
	std::string DottedName() const
	{
		std::string joined;
		for (const auto &segment : segments)
		{
			if (!joined.empty())
				joined += '.';
			joined += segment;
		}
		return joined;
	}
};

//A translation unit of an nlang source file. The import set (parsed
//`import` statements, see ImportSpec) belongs to the unit: one file's
//imports never make modules visible to another file in the project.
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

	//Take ownership of the import list (parsed `import` statements).
	void SetImports(std::vector<ImportSpec>* pImports)
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

	//Get the parsed `import` statements of this unit.
	const std::vector<ImportSpec>& Imports() const
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
	std::unique_ptr<std::vector<ImportSpec>> m_upImports;
	SnNamespace *m_pRoot;
	std::unordered_map<std::string, SnFieldExpr*> m_aliasTable;
	const std::string m_sFilePath;
};

} //namespace nlang
