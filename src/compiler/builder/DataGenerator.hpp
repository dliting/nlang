#ifdef NLANG_ENABLE_LLVM

#pragma once
#include "SnExtraTypes.h"
#include "VisitorBuilder.h"
#include "SyntaxTree.h"
#include <llvm/IR/Function.h>
#include <sstream>

namespace nlang
{

class DataGenerateAccessor : public SyntaxNodeBuildAccessor
{
	typedef SyntaxNodeBuildAccessor Super_;
public:
	explicit DataGenerateAccessor(BuildEnvironment &env) : Super_(env)
	{
	}

	void Access(SnNamespace &sn)
	{
		assert(m_pVisitor);
		for (auto &member : sn.Members())
			if (member.IsDataField() || CanBeFuncParent(member.Kind()))
				member.Accept(*m_pVisitor);
	}

	void Access(SnFunction &sn)
	{
		if (sn.IsImported())
		{
			//TODO: handle imported functions.
			assert(sn.MetaValue() || sn.IsExternal()); 
			return;
		}

		assert(!sn.MetaValue());

		//Build parameters.
		for (auto &param : sn.Params())
			param.Accept(*m_pVisitor);

		//Build meta function name.
		std::stringstream ss;
		ss << BuildPathName(sn);
		for (const auto &param : sn.Params())
			ss << '!' << param.Type()->MetaName();
		sn.MetaName(ss.str());

		//Build the return type.
		using namespace llvm;
		Type *pReturnType;
		if (!sn.ReturnType())
		{
			static auto s_pVoidType = Type::getVoidTy(m_Env.MetaContext());
			pReturnType = s_pVoidType;
		}
		else
			pReturnType = MetaTypeOf(*sn.ReturnType());

		assert(pReturnType);

		// Build the function type.
		std::vector<Type *> paramTypes(sn.Params().size());
		int i = 0;
		for (auto &param : sn.Params())
		{
			assert(param.MetaType());
			paramTypes[i] = param.MetaType();
			i++;
		}

		FunctionType *pFuncType =
			FunctionType::get(pReturnType, paramTypes, false);
		sn.m_pMetaType = pFuncType;

		//Create the function.
		auto linkType = GetFuncLinkType(sn);
		assert(m_Env.CurrMetaModule());
		Function *pMetaFunc = Function::Create(pFuncType, linkType,
			sn.MetaName(), m_Env.CurrMetaModule());

		//If pMetaFunc conflicted, there was something named 'sn.MetaName()'.
		assert(pMetaFunc->getName() == sn.MetaName());

		//If pMetaFunc already has a body, reject this.
		assert(pMetaFunc->empty());

		//Set parameter names.
		assert(pMetaFunc->arg_size() == sn.Params().size());
		auto iMetaParam = pMetaFunc->arg_begin();
		for (auto &param : sn.Params())
		{
			iMetaParam->setName(param.MetaName());
			param.m_pMetaValue = iMetaParam;
			++iMetaParam;
		}
		sn.m_pMetaValue = pMetaFunc;
	}

	void Access(SnFormalParam &sn)
	{
		sn.MetaName(sn.Name());
		assert(sn.Type());
		sn.m_pMetaType = MetaTypeOf(*sn.Type());
		//The meta value would be set by its parent function.
	}

	void Access(SyntaxNode &sn)
	{
		assert(false && "Not implemented.");
	}

	void Access(SnArrayTypeExpr &) {}
private:
	static std::string BuildPathName(const SnField &sn)
	{
		std::stringstream ss;
		BuildPathName(ss, sn);
		return std::move(ss.str());
	}

	static void BuildPathName(std::stringstream &ss, const SnField &sn)
	{
		static const SyntaxNode *pRoot = TheAST().Root();
		if (&sn == pRoot)
			return;

		assert(sn.Parent() && sn.Parent()->IsField());
		auto pParent = static_cast<const SnField*>(sn.Parent());
		if (pParent != pRoot)
		{
			BuildPathName(ss, *static_cast<const SnField*>(pParent));
			ss << ".";
		}
		ss << sn.Name();
	}

	static llvm::GlobalValue::LinkageTypes GetFuncLinkType(SnFunction &sn)
	{
		using namespace llvm;
		if (sn.AccessType() == FA_Private)
		{
			if (sn.ContainFlags(NF_Static))
				return Function::InternalLinkage;
			return Function::PrivateLinkage;
		}
		return Function::ExternalLinkage;
	}

	llvm::Type *MetaTypeOf(SnNameExpr &typeExpr)
	{
		auto pField = typeExpr.Field();
		assert(pField && pField->IsTypeField());
		return pField->MetaType();
	}
};

class DataGenerator : public SyntaxNodeVisitorBuilder<DataGenerateAccessor>
{
	typedef SyntaxNodeVisitorBuilder<DataGenerateAccessor> Super_;
public:
	explicit DataGenerator(BuildEnvironment &env) :
		Super_(env, NVK_CustomTraverse)
	{
	}

	void Execute(SnNamespace &root)
	{
		root.Accept(m_Visitor);
	}
};

} //namespace nlang

#endif // NLANG_ENABLE_LLVM