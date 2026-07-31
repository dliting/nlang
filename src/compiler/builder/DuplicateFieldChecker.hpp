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

	void Access(SyntaxNode &sn)
	{
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