/*-----------------------------------------------------------------------------
	ncomp/intf/ModuleBuilder.h
	This file define the interface of the module builder of nlang.
-----------------------------------------------------------------------------*/

#pragma once
#include "BuildEnvironment.h"
#include "SyntaxTree.h"
#include "nlang/vm/CompiledModule.h"
#include <list>
#include <memory>
#include <vector>

namespace nlang
{

//A tool for compiling some nlang source files to a binary module.
class NLANG_COMPILER_API ModuleBuilder
{
public:
	ModuleBuilder(const BuildParams&, CompileLogger&);

	~ModuleBuilder();

	//Compile source files to a module file.
	bool Build();
private:
	//Get the root of the syntax tree.
	SnNamespace &TreeRoot()
	{
		assert(TheAST().Root());
		return *TheAST().Root();
	}

	//Create a empty module as the current module to be built.
	bool CreateModule();
	//Parse source files.
	bool ParseSources();
	//Load imported symbols to a rebuilt AST.
	bool LoadImports();
	//Find .nmod file for a module name in m_ImportDirs. Returns empty if not found.
	std::string FindModuleFile(const std::string& name) const;
	//Generated executable codes.
	bool GenerateCodes();
	//Save the current module to a file.
	bool SaveModule();
	//Roughly parse the source files as translation units.
	void ParseTransUnits();
	//Merge the root fields in different translation units into the AST.
	void MergeTransUnits();

	void ResolveUsingLists();
	//Resolve type names of data fields in the syntax tree, but skip the 
	//function statements.
	void ResolveDataTypes();

	void CheckDuplicateFields();

	void ResolveDataValues();

	void ResolveStatements();

	//Check for circular struct references.
	void CheckStructCircularRefs();

		//Check for circular class inheritance.
		void CheckClassCircularInheritance();

	//Verify each class declaring "implements IFoo" provides all methods
	//declared in IFoo (matching name, params, return type). Reports errors
	//for missing methods.
	void CheckInterfaceImplementation();

	//Build LLVM data types.
	void GenerateTypeFields();

	void GenerateStatements();

	bool ResolveNameExpr(SnNameExpr &, SnField *pContext, 
		const SnField &accessor);

	bool ResolveIdentifierExpr(SnIdentifierExpr &, SnField *pContext, 
		const SnField &accessor);

	bool ResolveMemberExpr(SnMemberExpr &, SnField *pContext, 
		const SnField &accessor);

	SnField *FindFieldInAncestor(const std::string &sName, SyntaxNode &parent,
		const SnField &accessor);
	void GenerateDataFields();

	std::unique_ptr<BuildEnvironment> m_upEnv;
	std::unique_ptr<PtrList<TranslationUnit>> m_upTransUnits;
	//Cross-module import infrastructure (Phase 9c follow-up): compiled
	//modules loaded from .nmod files during LoadImports. Ownership is
	//transferred to VmBackend at the start of GenerateCodes via
	//SetImportedModules(); the backend then merges them into the user
	//module during GenerateStatements.
	std::vector<CompiledModule> m_loadedImports;
};

} //namespace nlang
