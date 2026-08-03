#pragma once
#include "SnExtraTypes.h"
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"

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
		CheckFields(sn.Members().NameDict());
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
private:
	DuplicateFieldCheckAccessor m_Accessor;
};

} //namespace nlang
