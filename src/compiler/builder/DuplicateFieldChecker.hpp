#pragma once
#include "SnExtraTypes.h"
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"
#include "BuiltinNames.h"
#include "TranslationUnit.h"
#include <nlang/vm/StdLib.h>
#include <set>
#include <string>

namespace nlang
{

//The class implements a visitor to fulfill duplicate field checking.
//This class should be used by \a DuplicateFieldChecker;
class DuplicateFieldCheckAccessor
{
	friend class DuplicateFieldChecker;
public:
	explicit DuplicateFieldCheckAccessor(BuildEnvironment &env) :
		m_Env(env), m_pVisitor(nullptr)
	{
	}

	void Access(SnNamespace &sn)
	{
		CheckFields(sn.Members().NameDict());
		for (auto &member : sn.Members())
			member.Accept(*m_pVisitor);
	}

	void Access(SnFunction &sn)
	{
		if (sn.IsImported())
			return;
		CheckFields(sn.Params().NameDict());
	}

	void Access(SnEnumDecl &sn)
	{
		/*
		Phase 12: enum methods live in a separate kind-filtered list.
		Check each table for internal duplicates, then across the two
		tables (a method sharing a member's name is a conflict — the
		member would shadow the method in FindField). CheckFields Accepts
		every field, so the methods' param checks run through their own
		NameDict pass — no separate descend loop needed. (Param
		diagnostics are multi-logged by the pre-existing pass machinery,
		same as class methods.)
		*/
		CheckFields(sn.Members().NameDict());
		CheckFields(sn.Methods().NameDict());
		for (auto &method : sn.Methods()) {
			auto *pMember = sn.Members().find(method.Name());
			if (pMember) {
				m_Env.Log(CLL_Error, method.Location(),
					"The field \"%s\" is conflicted with a exist field "
					"definition.", method.ToString().c_str());
				m_Env.Log(CLL_More, pMember->Location(),
					"See also the definition of \"%s\".",
					pMember->ToString().c_str());
			}
		}
	}

	void Access(SnStructDecl &sn)
	{
		CheckFields(sn.Members().NameDict());
	}

	void Access(SnClassDecl &sn)
	{
		CheckFields(sn.Members().NameDict());
		//Check that child fields don't hide inherited fields.
		auto *pSuper = sn.SuperClass();
		while (pSuper) {
			for (auto &member : pSuper->Members()) {
				if (member.Kind() != NK_ClassField)
					continue;
				auto range = sn.Members().NameDict().equal_range(member.Name());
				for (auto it = range.first; it != range.second; ++it) {
					if (it->second->Kind() == NK_ClassField) {
						m_Env.Log(CLL_Error, it->second->Location(),
							"The field \"%s\" hides an inherited field from \"%s\".",
							member.Name().c_str(), pSuper->Name().c_str());
						break;
					}
				}
			}
			pSuper = pSuper->SuperClass();
		}
		//Traverse members (e.g. SnFunction for param duplicate checks).
		for (auto &member : sn.Members())
			member.Accept(*m_pVisitor);
	}

	void Access(SnInterfaceDecl &sn)
	{
		//Check for duplicate method declarations in the interface body.
		CheckFields(sn.Members().NameDict());
		//Traverse members (SnFunction for param duplicate checks).
		for (auto &member : sn.Members())
			member.Accept(*m_pVisitor);
	}

	void Access(SyntaxNode &sn)
	{
	}

	void Access(SnArrayTypeExpr &)
	{
		//Array type expressions introduce no new fields to check.
	}

	void Access(SnAsExpr &)
	{
		//Phase 8e-1.5: `expr as T` introduces no fields to check.
	}

	void Access(SnGenericTypeExpr &)
	{
		//Phase 8e-3: `List<T>` introduces no fields to check.
	}
private:
	template <class NAME_DICT_T>
	void CheckFields(const NAME_DICT_T &nameMap)
	{
		//Compare fields ordered by name.
		auto iEnd = nameMap.end();
		auto iFirstPrev			= iEnd; //The first previous field with the same name.
		std::string	sPrevName	= "//"; //The previous field name.
		for (auto iCurr = nameMap.begin(); iCurr != iEnd; ++iCurr)
		{
			SnField *pCurrField = iCurr->second;
			//Phase 11: math/io/fs are reserved as stdlib namespaces so the
			//resolver can route `math.sqrt(x)` on the outer name alone.
			//Every NameDict (namespace/class/struct/enum members, function
			//params) flows through here — one choke point for all of them.
			if (IsStdLibNamespaceName(pCurrField->Name()))
			{
				m_Env.Log(CLL_Error, pCurrField->Location(),
					"The name \"%s\" is reserved for a standard library "
					"namespace.", pCurrField->Name().c_str());
			}
			if (sPrevName == pCurrField->Name())
			{
				//Find the first field conflicted with the current one.
				assert(iFirstPrev != iEnd);
				auto iPrev = iFirstPrev;
				do
				{
					SnField *pPrevField = iPrev->second;
					if (DetectConflict(*pCurrField, *pPrevField))
						break;
					++iPrev;
				} while (iPrev != iCurr);
			}
			else
			{
				sPrevName = pCurrField->Name();
				iFirstPrev = iCurr;
			}
			pCurrField->Accept(*m_pVisitor);
		}
	}

	bool DetectConflict(const SnField &f1, const SnField &f2)
	{
		if (!f1.ConflictedWith(f2))
			return false;
		auto &errorField = (f1.IsImported()) ? f2 : f1;
		auto &existField = (&errorField == &f1) ? f2 : f1;
		m_Env.Log(CLL_Error, errorField.Location(),
			"The field \"%s\" is conflicted with a exist field definition.",
			errorField.ToString().c_str());
		m_Env.Log(CLL_More, existField.Location(),
			"See also the definition of \"%s\".",
			existField.ToString().c_str());
		return true;
	}

	BuildEnvironment &m_Env;
	ISyntaxNodeVisitor *m_pVisitor;
};

//The helper class to detect conflict fields.
//The fields defined in imported nodes or in statements will not be checked.
class DuplicateFieldChecker
{
public:
	explicit DuplicateFieldChecker(BuildEnvironment &env) : m_Accessor(env)
	{
	}

	//Check all fields in a namespace except those declared in statements.
	void Check(SnNamespace &root)
	{
		SyntaxNodeVisitor<DuplicateFieldCheckAccessor> visitor(m_Accessor);
		m_Accessor.m_pVisitor = &visitor;
		root.Accept(visitor);
	}

	/*
	Phase 13: check the alias-form usings of one translation unit for name
	clashes. The comparison is deliberately TU-local: it must run BEFORE the
	units are merged (unit roots intact), because an alias in TU1 may legally
	share its name with a member of TU2 — TU1's use sites are already
	expanded by then. Compared sets: other aliases of the same unit, the
	unit's own root members, built-in type names in the tree root (int/
	float/string), lazily synthesized built-in class names, the built-in
	generic class names, and the reserved stdlib namespace names.
	*/
	void CheckUnitAliases(const TranslationUnit &unit, SnNamespace &treeRoot)
	{
		auto pUsings = unit.Usings();
		if (!pUsings)
			return;

		std::set<std::string> aliasNames;
		for (auto pUsing : *pUsings)
		{
			if (!pUsing->IsAlias())
				continue;
			const auto sName = pUsing->AliasName();
			if (!aliasNames.insert(sName).second)
			{
				m_Accessor.m_Env.Log(CLL_Error, pUsing->Location(),
					"The type alias \"%s\" is defined more than once in "
					"this translation unit.", sName.c_str());
				continue;
			}
			//Names that resolution would route somewhere else than the
			//alias: same-unit declarations, built-in types and classes,
			//built-in generics, reserved stdlib namespaces.
			if ((unit.Root() && unit.Root()->FindField(sName))
				|| treeRoot.FindField(sName)
				|| sName == "List" || sName == "Dict" || sName == "Func"
				|| IsBuiltinClassName(sName)
				|| IsStdLibNamespaceName(sName))
			{
				m_Accessor.m_Env.Log(CLL_Error, pUsing->Location(),
					"The name \"%s\" cannot be used as a type alias.",
					sName.c_str());
			}
		}
	}
private:
	DuplicateFieldCheckAccessor m_Accessor;
};

} //namespace nlang
