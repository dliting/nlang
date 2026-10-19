/*---
    builder/ClassFieldInitializerInjection.cpp — class 字段初始化器的构造器
    注入（0.9.11 D4/D9）。声明期检查（FieldInitializerChecks.hpp）接受
    `Type name = <字面量>;` 后，本通道把默认值的应用合成为
    `this.name = <克隆>;` 赋值语句，注入声明类的每个构造器（super() 居首
    则其后，否则体首）；类无用户构造器时合成 public 零参构造器承载注入。
    由 StatementResolver::PreInjectClassFieldInitializers 预通道驱动，先于
    文档序遍历，使先于类声明书写的构造器调用点也能看到合成成员。每个类
    两阶段：先验证全部构造器（native/无体拒绝、super 首位规则），任一诊断
    即整类零 AST 改动，不留半成品默认值。
---*/
#include "StatementResolver.h"
#include "FieldInitializerChecks.hpp"
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnStatements.h>
#include <vector>

namespace nlang
{

namespace
{

//0.9.11 D9: clone a foldable initializer into a fresh literal node at
//the initializer's own location, always through the RTTI template ctor
//in the grammar's own shape. Numeric forms rebuild from the folded
//value (a unary negation is absorbed by the fold); bool/char/string
//literals rebuild from their literal payload. Null = outside the fold
//surface: the declaration-time check reports it, injection skips the
//field.
SnExpression *CloneConstantInitializer(SnExpression *pInit)
{
	if (!pInit)
		return nullptr;
	const ConstantFoldKind foldKind = ClassifyConstantInitializer(pInit);
	const ISourceLocation &loc = *pInit->Location();
	switch (foldKind)
	{
	case ConstantFoldKind::Null:
	{
		//null binds a scalar/string slot as its zero sentinel, the same
		//literal shape the grammar builds.
		auto *pNull = new SnLiteralExpr(*RnInt32::Instance(), 0, loc);
		pNull->AddFlags(NF_NullLiteral);
		return pNull;
	}
	case ConstantFoldKind::Bool:
	case ConstantFoldKind::Char:
	case ConstantFoldKind::String:
	{
		//ClassifyConstantInitializer admits these only as literal
		//nodes. Rebuild through the RTTI template ctor in the grammar's
		//own shape (precedent: SynthScalarDefault). The Variant copy
		//ctor is deliberately not used: RnString::CopyValue writes
		//*through* the destination pointer, which is uninitialized
		//storage in a fresh Variant (crashes on the first string clone).
		Variant &src = static_cast<SnLiteralExpr *>(pInit)->Value();
		if (foldKind == ConstantFoldKind::Bool)
			return new SnLiteralExpr(*RnBool::Instance(),
				src.Data().m_Int, loc);
		if (foldKind == ConstantFoldKind::Char)
			return new SnLiteralExpr(*RnChar::Instance(),
				static_cast<uint32>(src.Data().m_Int), loc);
		return new SnLiteralExpr(*RnString::Instance(),
			new std::string(*src.Data().m_String), loc);
	}
	case ConstantFoldKind::Int32:
	case ConstantFoldKind::Long:
	case ConstantFoldKind::ULong:
	case ConstantFoldKind::Float:
	case ConstantFoldKind::Double:
	{
		ConstLiteralValue value;
		if (!TryGetConstantLiteral(pInit, value))
			return nullptr;	//the classifier already agreed foldable
		switch (value.kind)
		{
		case NK_Int32:
			return new SnLiteralExpr(*RnInt32::Instance(),
				static_cast<int32>(value.i), loc);
		case NK_Long:
			return new SnLiteralExpr(*RnLong::Instance(),
				static_cast<int64>(value.i), loc);
		case NK_ULong:
			return new SnLiteralExpr(*RnULong::Instance(),
				static_cast<uint64>(value.u), loc);
		case NK_Float:
			return new SnLiteralExpr(*RnFloat::Instance(),
				static_cast<float>(value.d), loc);
		case NK_Double:
			return new SnLiteralExpr(*RnDouble::Instance(),
				value.d, loc);
		default:
			return nullptr;
		}
	}
	default:
		return nullptr;
	}
}

//Fields carrying an initializer, in declaration order.
void CollectFieldInitFields(SnClassDecl &sn,
	std::vector<SnClassField *> &initFields)
{
	for (auto &member : sn.Members())
	{
		if (member.Kind() == NK_ClassField)
		{
			auto &field = static_cast<SnClassField &>(member);
			if (field.Initializer())
				initFields.push_back(&field);
		}
	}
}

} //anonymous namespace

//User constructors (instance members named after the class). Returns
//false when the class carries a generated constructor, i.e. it was
//already injected and this pass must stay a no-op.
bool StatementResolveAccessor::CollectUserCtors(SnClassDecl &sn,
	std::vector<SnFunction *> &ctors)
{
	for (auto &member : sn.Members())
	{
		if (member.Kind() != NK_Function)
			continue;
		auto &func = static_cast<SnFunction &>(member);
		if (func.ContainFlags(NF_Generated))
			return false;
		if (IsConstructorFunction(func))
			ctors.push_back(&func);
	}
	return true;
}

//Phase 1 check for one constructor (D9): a native or bodyless
//constructor has no statement list to inject into, and with
//initializers present super() must be the first statement — the
//injection anchor is undefined otherwise. Diagnostics are emitted
//here; the caller aborts the whole class on any rejection.
bool StatementResolveAccessor::ValidateCtorForFieldInit(SnFunction &ctor)
{
	if (!ctor.Body() || ctor.ContainFlags(NF_Native))
	{
		m_Env.Log(CLL_Error, ctor.Location(),
			"class field initializers cannot be applied to native "
			"or bodyless constructors");
		return false;
	}
	auto &stmts = ctor.Body()->Statements();
	SnStatement *pSuper = nullptr;
	int superIndex = -1;
	int index = 0;
	for (auto &stmt : stmts)
	{
		if (stmt.Kind() == NK_SuperCallStmt)
		{
			pSuper = &stmt;
			superIndex = index;
			break;
		}
		++index;
	}
	if (superIndex > 0)
	{
		m_Env.Log(CLL_Error, pSuper->Location(),
			"field initializers require super() to be the first "
			"statement of the constructor");
		return false;
	}
	return true;
}

//Inject one `this.<field> = <clone>;` per initializer, in declaration
//order, right after a leading super() call or at body start. Fresh
//nodes per constructor (D9): each constructor owns its statement tree,
//so no node is ever shared between two bodies. Every assignment is
//inserted at one fixed anchor (the original successor of the injection
//point): list insertion places each new node right before the anchor,
//i.e. after the previously injected ones, keeping declaration order
//(same anchor discipline as RegisterLocalDeclarators).
void StatementResolveAccessor::InjectIntoCtor(SnFunction &ctor,
	const std::vector<SnClassField *> &initFields)
{
	SnParagraph &body = *ctor.Body();
	auto pos = body.Children().begin();
	if (pos != body.Children().end() && pos->Kind() == NK_SuperCallStmt)
		++pos;
	for (auto *pField : initFields)
	{
		SnExpression *pClone = CloneConstantInitializer(
			pField->Initializer());
		if (!pClone)
			continue;	//non-foldable: the declaration check reports it
		const ISourceLocation &loc = *pClone->Location();
		auto *pThis = new SnThisExpr(loc);
		auto *pName = new SnIdentifierExpr(
			new std::string(pField->Name()), loc);
		auto *pMember = new SnMemberExpr(pThis, pName, loc);
		auto *pAssign = new SnAssignStmt(pMember, pClone, loc);
		pAssign->AddFlags(NF_Generated);
		body.InsertChild(pos, pAssign);
	}
}

//0.9.11 D9: apply one class's field initializers. Phase 1 validates
//every constructor (native/bodyless rejection, super-position rule);
//any diagnostic leaves the AST untouched. Phase 2 injects into each
//constructor, or synthesizes the public zero-arg constructor when the
//class declares none. The pre-pass visits each class once per Resolve;
//the generated-constructor guard keeps the pass idempotent if it ever
//re-runs.
void StatementResolveAccessor::InjectClassFieldInitializers(SnClassDecl &sn)
{
	std::vector<SnClassField *> initFields;
	CollectFieldInitFields(sn, initFields);
	if (initFields.empty())
		return;

	std::vector<SnFunction *> ctors;
	if (!CollectUserCtors(sn, ctors))
		return;	//already injected by an earlier pass

	//Phase 1: validate every constructor; any diagnostic aborts the
	//whole class (no half-applied defaults).
	bool bRejected = false;
	for (auto *pCtor : ctors)
		if (!ValidateCtorForFieldInit(*pCtor))
			bRejected = true;
	if (bRejected)
		return;

	//Phase 2: inject into each user constructor.
	for (auto *pCtor : ctors)
		InjectIntoCtor(*pCtor, initFields);
	if (!ctors.empty())
		return;

	//No user constructor: synthesize the public zero-arg form so
	//`Cfg()`, `Cfg{}` and `new Cfg()` all reach the defaults.
	auto *pSynth = new SnFunction(FA_Public, NF_NONE, nullptr,
		new std::string(sn.Name()), UniquePtrList<SnFormalParam>(),
		*sn.Location());
	pSynth->AddFlags(NF_Generated);
	pSynth->Body(new SnParagraph(UniquePtrList<SnStatement>(),
		*sn.Location()));
	InjectIntoCtor(*pSynth, initFields);
	sn.Members().push_back(pSynth);
}

} //namespace nlang
