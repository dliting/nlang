/*-----------------------------------------------------------------------------
ncomp/intf/MetaTypeBuilder.hpp
This file define a LLVM type builder for the AST.
-----------------------------------------------------------------------------*/

#ifdef NLANG_ENABLE_LLVM

#pragma once
#include "SyntaxTree.h"
#include "VisitorBuilder.h"
#include <llvm/IR/DerivedTypes.h>

namespace nlang
{

/*
LLVM type builder for an nlang syntax tree.
This class is an Accessor used with a \a SyntaxNodeVisitor
*/
class TypeGenerateAccessor : public SyntaxNodeBuildAccessor
{
	typedef SyntaxNodeBuildAccessor Super_;
public:
	explicit TypeGenerateAccessor(BuildEnvironment &env) : Super_(env)
	{
	}

	void Access(SnNamespace &sn)
	{
		assert(m_pVisitor);
		for (auto &member : sn.Members())
			if (member.IsTypeField())
				member.Accept(*m_pVisitor);
		sn.m_pMetaType = GetPointerType();
		//TODO: sn.m_pMetaValue =
	}

	void Access(SnInt32 &sn)
	{
		sn.m_pMetaType = llvm::Type::getInt32Ty(m_Env.MetaContext());
		//TODO: sn.m_pMetaValue = PointerTo(SnInt32::Class());
		sn.MetaName(sn.Name());
	}

	void Access(SnString &sn)
	{
		sn.m_pMetaType = GetPointerType();
		sn.MetaName(sn.Name());
	}

	void Access(SnType &sn)
	{
		sn.m_pMetaType = GetPointerType();
		sn.MetaName(sn.Name());
	}

	//Default action.
	void Access(SyntaxNode &sn)
	{
		assert(false && "Require implementation.");
	}

	void Access(SnArrayTypeExpr &) {}

	//Phase 8e-1.5: `expr as T` is not a runtime type declaration.
	void Access(SnAsExpr &) {}

	//Phase 8e-3: `List<T>` is not a runtime type declaration (VM-registered).
	void Access(SnGenericTypeExpr &) {}

	//Interfaces are not concrete runtime types (no heap layout); classes that
	//implement them carry the runtime representation. Skip type generation.
	void Access(SnInterfaceDecl &) {}

	llvm::Type *GetPointerType()
	{
		static auto ptrType = llvm::Type::getInt8PtrTy(m_Env.MetaContext());
		return ptrType;
	}
};

class TypeGenerator : public SyntaxNodeVisitorBuilder<TypeGenerateAccessor>
{
	typedef SyntaxNodeVisitorBuilder<TypeGenerateAccessor> Super_;
public:
	explicit TypeGenerator(BuildEnvironment &env) : 
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
