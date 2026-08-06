/*-----------------------------------------------------------------------------
ncomp/impl/builder/StatementGenerator.hpp
This file define the interface and the implementation of a LLVM code generator
for statements in an nlang AST.
-----------------------------------------------------------------------------*/
#ifdef NLANG_ENABLE_LLVM

#pragma once
#include "SnExpressions.h"
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/IR/Constants.h>

namespace nlang
{

//Scope guard with a callback function.
class ScopeGuard
{
public:
	explicit ScopeGuard(const std::function<void()> &exitAction) :
		m_ExitAction(exitAction)
	{
	}

	~ScopeGuard()
	{
		m_ExitAction();
	}
private:
	std::function<void()> m_ExitAction;
};

#define ON_EXIT_SCOPE(f) ScopeGuard sg##__LINE__##_(f);


//A syntax node accessor to generate LLVM function implementation codes.
class StatementGenerateAccessor
{
	friend class StatementGenerator;
public:
	explicit StatementGenerateAccessor(BuildEnvironment &env) :
		m_Env(env), m_pVisitor(nullptr), m_pCurrFunc(nullptr),
		m_IRBuilder(m_Env.MetaContext())
	{
	}

	void Access(SnNamespace &sn)
	{
		assert(m_pVisitor);
		for (auto &member : sn.Members())
			if (CanBeFuncParent(member.Kind()) || member.Kind() == NK_Function)
				member.Accept(*m_pVisitor);
	}

	void Access(SnFunction &sn)
	{
		if (sn.IsImported())
			return;

		assert(!sn.IsExternal()); //TODO

		m_pCurrFunc = &sn;
		ON_EXIT_SCOPE([this]{ m_pCurrFunc = nullptr; });

		using namespace llvm;
		auto pMetaFunc = sn.MetaFunc();
		assert(pMetaFunc && sn.Body());
		BasicBlock *pBB = 
			BasicBlock::Create(m_Env.MetaContext(), "entry", pMetaFunc);
		m_IRBuilder.SetInsertPoint(pBB);
		for (auto &stmt : sn.Body()->Statements())
			stmt.Accept(*m_pVisitor);
		verifyFunction(*pMetaFunc);
	}

	void Access(SnReturnStmt &sn)
	{
		assert(m_pCurrFunc);
		auto pResult = sn.Result();
		if (!pResult)
		{
			assert(!m_pCurrFunc->ReturnType());
			m_IRBuilder.CreateRetVoid();
			return;
		}

		assert(m_pCurrFunc->ReturnType());
		assert(m_pCurrFunc->ReturnType()->Field() == pResult->EvalDataType());
		pResult->Accept(*m_pVisitor);
		m_IRBuilder.CreateRet(pResult->MetaValue());
	}

	void Access(SnNameExpr &sn)
	{
		sn.Expr()->Accept(*m_pVisitor);
		sn.m_pMetaValue = sn.Expr()->MetaValue();
	}

	void Access(SnMemberExpr &sn)
	{
		auto pOuterExpr	= sn.Outer();
		auto pInnerExpr	= sn.Inner();
		assert(pOuterExpr && pInnerExpr);
		//TODO: generate member access code.
		pOuterExpr->Accept(*m_pVisitor);
		pInnerExpr->Accept(*m_pVisitor);
		sn.m_pMetaValue = pInnerExpr->MetaValue();
	}

	void Access(SnLiteralExpr &sn)
	{
		using namespace llvm;
		switch (sn.Value().Type()->Kind())
		{
		case NK_Int32: 
			{
				const int32 v = sn.Value().Get<int32>();
				auto metaType = Type::getInt32Ty(m_Env.MetaContext());
				sn.m_pMetaValue = ConstantInt::getSigned(metaType, v);
			}
			break;
		default:
			assert(false); //TODO: string, etc.
			break;
		}
	}

	void Access(SnIdentifierExpr &sn)
	{
		assert(sn.Field());
		sn.m_pMetaValue = sn.Field()->MetaValue();
	}

	void Access(SnInvokeExpr &sn)
	{
		SnFunction *pCallee = sn.Callee();
		assert(pCallee && pCallee->MetaValue());

		std::vector<llvm::Value*> metaParams;
		for (auto &param : sn.Params()) 
		{
			param.Accept(*m_pVisitor);
			metaParams.push_back(param.MetaValue());
		}

		sn.m_pMetaValue = m_IRBuilder.CreateCall(
			pCallee->MetaValue(), metaParams, "calltmp");
	}

	void Access(SnCastExpr &sn)
	{
		//TODO
	}

	void Access(SnAsExpr &sn)
	{
		//TODO — LLVM IR path not used by VM backend.
	}

	//Default action.
	void Access(SyntaxNode &sn)
	{
		assert(false && "invalid node type");
	}

	void Access(SnArrayTypeExpr &) {}

	//Interface declarations have no statements to generate.
	void Access(SnInterfaceDecl &) {}
private:
	BuildEnvironment &m_Env;
	ISyntaxNodeVisitor *m_pVisitor;
	llvm::IRBuilder<> m_IRBuilder;
	//The current function being visited.
	SnFunction *m_pCurrFunc; 
};

class StatementGenerator
{
public:
	explicit StatementGenerator(BuildEnvironment &env) : m_Accessor(env)
	{
	}

	void Execute(SnNamespace &root)
	{
		SyntaxNodeVisitor<StatementGenerateAccessor> 
			visitor(m_Accessor, NVK_CustomTraverse);
		m_Accessor.m_pVisitor = &visitor;
		root.Accept(visitor);
	}
private:
	StatementGenerateAccessor m_Accessor;
};

} //namespace nlang

#endif // NLANG_ENABLE_LLVM