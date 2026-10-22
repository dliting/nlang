/*---
    StatementResolverDeclArray.cpp — C 风格定长数组声明的类型重塑与合成降级：
    `T name[n];` 分解为 `name = new T[n]`；`T name[n] = [e1, e2];` 追加
    每个条目一条下标赋值，其余槽位保持创建默认值。
    自 StatementResolverDecls.cpp 抽出（0.9.0，per-concern TU split，
    源文件 500 行守卫），与 StatementResolverDeclCtor.cpp 同模式。
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"
#include <nlang/runtime/RnTypes.h>
#include <vector>

namespace nlang
{

//Partial-init validation of BuildSizedArrayDeclAssigns: the initializer
//must be the bare `[...]` literal form (`= 5` or `= new int[2]` has no
//per-entry meaning for a fixed-size array), and when the size folds to
//an integer constant the entry count must fit (D9). False = rejected
//(diagnostic logged, pInitList left null, nothing detached).
static bool ValidateSizedArrayInit(BuildEnvironment &env,
	SnLocalDeclStmt::LocalDecl &decl, SnInitListExpr* &pInitList)
{
	pInitList = nullptr;
	if (!decl.pInitExpr)
		return true;
	if (decl.pInitExpr->Kind() != NK_InitListExpr
		|| !static_cast<SnInitListExpr *>(decl.pInitExpr)->IsArrayForm())
	{
		env.Log(CLL_Error, decl.pInitExpr->Location(),
			"sized declaration initializer must be a [...] literal");
		return false;
	}
	pInitList = static_cast<SnInitListExpr *>(decl.pInitExpr);
	//More literal elements than the declared size cannot fit —
	//rejected statically when the size folds to an integer constant.
	ConstLiteralValue size;
	if (TryGetConstantLiteral(decl.pArraySize, size)
		&& size.kind == NK_Int32
		&& static_cast<int64_t>(pInitList->Entries().size()) > size.i)
	{
		env.Log(CLL_Error, pInitList->Location(),
			"initializer has %zu element(s), more than the declared "
			"size %lld",
			pInitList->Entries().size(),
			static_cast<long long>(size.i));
		pInitList = nullptr;
		return false;
	}
	return true;
}

//Entry assigns of the partial-init form: one `name[i] = entry` per
//literal element, shaped as the dedicated SnSubscriptAssignStmt store
//(a plain AssignStmt over a subscript LHS would emit a value read, not
//a store). Entries detach from the init-list shell (its children list
//owns them) and re-home into the synthesized statements; the caller
//deletes the emptied shell after detaching it from the declaration.
static void AppendSizedEntryAssigns(SnLocalDeclStmt &sn,
	SnLocalDeclStmt::LocalDecl &decl, SnInitListExpr &initList,
	std::vector<SnStatement *> &assigns)
{
	const auto &entries = initList.Entries();
	for (size_t i = 0; i < entries.size(); ++i)
	{
		if (!entries[i].pValue)
			continue;
		auto *pIndex = new SnLiteralExpr(*RnInt32::Instance(),
			static_cast<int32_t>(i), *sn.Location());
		assigns.push_back(new SnSubscriptAssignStmt(
			new SnIdentifierExpr(new std::string(decl.name),
				*sn.Location()),
			pIndex, initList.DetachChild(entries[i].pValue),
			*sn.Location()));
	}
}

std::vector<SnStatement *> StatementResolveAccessor::BuildSizedArrayDeclAssigns(
	SnLocalDeclStmt &sn, SnLocalDeclStmt::LocalDecl &decl,
	SnField *pSizedElemField)
{
	std::vector<SnStatement *> assigns;
	//The jagged (or otherwise unshapable) declaration was diagnosed at
	//the type re-shape; nothing to synthesize, nothing detached.
	if (!pSizedElemField)
		return assigns;
	SnInitListExpr *pInitList = nullptr;
	if (!ValidateSizedArrayInit(m_Env, decl, pInitList))
		return assigns;
	//Creation: `name = new T[size]`. The fresh element field binds the
	//ALREADY-RESOLVED element type directly (ResolveFieldExprAs) — the
	//same bypass BuildCtorInitAssign uses for the class name:
	//value-position lookup could find shadowing locals, and built-in
	//element types have no namespace entry to find.
	auto *pElemType = new SnIdentifierExpr(
		new std::string(sn.Type()->ToString()), *sn.Location());
	m_ExprResolver.ResolveFieldExprAs(*pElemType, pSizedElemField);
	auto *pNew = new SnNewArrayExpr(pElemType,
		sn.DetachChild(decl.pArraySize), *sn.Location());
	assigns.push_back(new SnAssignStmt(
		new SnIdentifierExpr(new std::string(decl.name), *sn.Location()),
		pNew, *sn.Location()));
	if (pInitList)
	{
		AppendSizedEntryAssigns(sn, decl, *pInitList, assigns);
		sn.DetachChild(decl.pInitExpr);
		delete pInitList;
	}
	return assigns;
}

void StatementResolveAccessor::SynthesizeSizedArrayDeclarator(
	SnLocalDeclStmt &sn, SnLocalDeclStmt::LocalDecl &decl,
	SnParagraph &paragraph, NodeIterator iInsert,
	SnField *pSizedElemField)
{
	auto assigns = BuildSizedArrayDeclAssigns(sn, decl, pSizedElemField);
	//Both payload slots are owning children of sn: consumed (detached)
	//on success, tree-freed on rejection — drop the aliases either way
	//so codegen never re-reads them.
	decl.pArraySize = nullptr;
	decl.pInitExpr = nullptr;
	for (auto *pAssign : assigns)
	{
		paragraph.InsertChild(iInsert, pAssign);
		pAssign->Accept(*m_pVisitor);
	}
}

SnField *StatementResolveAccessor::BindSizedArrayDeclType(SnLocalDeclStmt &sn)
{
	//0.10.6 D2: each declarator carries its own brackets — a size
	//payload types just that declarator as an array, siblings keep the
	//head type (`int a[3], b;` declares b as a plain int). The head
	//type expression is never re-bound; the per-declarator array field
	//is interned at registration and flagged on the payload for
	//codegen (LocalDecl::bArrayType).
	for (auto &decl : sn.Decls())
	{
		if (!decl.pArraySize)
			continue;
		if (sn.Type()->IsArrayType())
		{
			//`int[] a[3]` is jagged: same text as the nested-element
			//rejection in Access(SnNewArrayExpr&) — the synthesis's
			//fresh element field loses the SnArrayTypeExpr shape, so
			//that guard cannot fire on this path.
			m_Env.Log(CLL_Error, sn.Location(),
				"Multi-dimensional arrays are not supported.");
			return nullptr;
		}
		if (!sn.Type()->Field())
			return nullptr;  //resolved without a field: nothing to bind
		return sn.Type()->Field();
	}
	return nullptr;
}

} //namespace nlang
