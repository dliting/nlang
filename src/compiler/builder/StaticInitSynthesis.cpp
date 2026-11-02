/*---
    StaticInitSynthesis.cpp — v0.12.6 D2/D4 (spec §6.2): synthesize each
    class's lazy static-initializer member before the document-order
    traversal. A class needs one iff it declares a static field WITH an
    initializer or its super chain does (the chain-only case carries an
    empty body — the backend still emits the super gate+call prefix, so
    construction of a subclass touches the parent first). The
    initializer expressions move out of the field nodes into plain
    assignment statements inside a "$staticInit" static member function,
    so the ordinary statement/expression machinery resolves them (bare
    names, implicit-this rejection, diagnostics) with zero special cases.
    The document-order function loop then resolves the member like any
    static method; the field keeps its non-owning Initializer() pointer
    (the node stays alive under the assignment it owns).
---*/
#include "StatementResolver.h"
#include "BuiltinNames.h"
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nlang {

namespace {

//Static fields with initializers, in declaration order.
void CollectInitializedStatics(SnClassDecl& decl,
	std::vector<SnClassField*>& out)
{
	for (auto& member : decl.Members()) {
		if (member.Kind() != NK_ClassField)
			continue;
		auto& field = static_cast<SnClassField&>(member);
		if (field.ContainFlags(NF_Static) && field.Initializer())
			out.push_back(&field);
	}
}

//D4 chain rule with memo: a class needs $staticInit when it or any
//ancestor declares an initialized static. Bases are linked by
//PreResolveClassBases (which rejects inheritance cycles), so the walk
//terminates; the memo keeps deep chains linear.
bool ClassNeedsStaticInit(SnClassDecl& decl,
	std::unordered_map<SnClassDecl*, bool>& memo)
{
	auto found = memo.find(&decl);
	if (found != memo.end())
		return found->second;
	std::vector<SnClassField*> own;
	CollectInitializedStatics(decl, own);
	const bool needs = !own.empty()
		|| (decl.SuperClass()
			&& ClassNeedsStaticInit(*decl.SuperClass(), memo));
	memo[&decl] = needs;
	return needs;
}

//Build one class's "$staticInit" member: `field = <initializer>;` per
//initialized static. The initializer node is DETACHED from its FIELD
//(the field's child list owns it — the class decl does not) so the tree
//walk visits it exactly once (containment invariant), and the
//assignment carries NF_Generated to keep it out of user-facing
//diagnostics. The field's non-owning Initializer() pointer stays valid:
//the node is alive under the assignment that now owns it.
void SynthesizeOneClass(SnClassDecl& decl)
{
	std::vector<SnClassField*> fields;
	CollectInitializedStatics(decl, fields);
	const ISourceLocation& loc = *decl.Location();

	UniquePtrList<SnStatement> stmts;
	for (auto* field : fields) {
		//The collect filter guarantees a non-null initializer, and
		//DetachChild asserts the field owns it — wrong-owner is a bug,
		//not a tolerated miss.
		SnExpression* init = field->DetachChild(field->Initializer());
		NLANG_ASSERT(init);
		auto* lhs = new SnIdentifierExpr(
			new std::string(field->Name()), *init->Location());
		auto* assign = new SnAssignStmt(lhs, init, *init->Location());
		assign->AddFlags(NF_Generated);
		stmts.Add(assign);
	}

	auto* func = new SnFunction(FA_Public,
		NF_Static | NF_Generated | NF_ExplicitVoid, nullptr,
		new std::string(kStaticInitFuncName),
		UniquePtrList<SnFormalParam>(), loc);
	func->Body(new SnParagraph(std::move(stmts), loc));
	decl.Members().push_back(func);
}

//D12 layer 1: a class's static-field initializers execute in
//declaration order, so a DIRECT reference to a later-declared static of
//the SAME class reads the not-yet-stored slot (the silent zero D12
//eliminates). Bare and Class.member spellings are diagnosable here by
//spelling — the initializer expression has no locals to shadow the
//field, and class-member names are unique, so a bare name matching a
//later static field can only resolve to it. Indirection through calls
//is the runtime init-bit layer's job (the cycle probe).
void ReportForwardRef(BuildEnvironment& env, const std::string& name,
                      const ISourceLocation& loc)
{
	env.Log(CLL_Error, &loc,
		"The static field \"%s\" is referenced before its declaration "
		"initializes it.",
		name.c_str());
}

void ScanForForwardRefs(Node& node, const std::string& className,
	const std::unordered_set<std::string>& later,
	BuildEnvironment& env)
{
	if (node.Kind() == NK_MemberExpr) {
		auto& me = static_cast<SnMemberExpr&>(node);
		//Class.member: the self-class qualified form reads this class's
		//own later static — diagnose once here; the outer identifier is
		//the type reference and the inner identifier names the member,
		//so neither side re-walks the bare arm (no duplicate report).
		const auto* inner = dynamic_cast<SnIdentifierExpr*>(me.Inner());
		if (inner && me.Outer()->Kind() == NK_IdentifierExpr) {
			const auto& outer =
				static_cast<SnIdentifierExpr&>(*me.Outer());
			if (outer.Name() == className && later.count(inner->Name())) {
				ReportForwardRef(env, inner->Name(), *me.Location());
				return;
			}
		}
		//Any other member access: the outer side evaluates a value (a
		//real read — recurse with the bare arm). A plain inner
		//identifier names the TARGET scope's member, not this class's
		//field (Class.field of a foreign Class must not fire), so only
		//non-identifier inner nodes recurse (a method call's arguments).
		ScanForForwardRefs(*me.Outer(), className, later, env);
		if (me.Inner() && !inner)
			ScanForForwardRefs(*me.Inner(), className, later, env);
		return;
	}
	if (node.Kind() == NK_IdentifierExpr) {
		const auto& id = static_cast<SnIdentifierExpr&>(node);
		if (later.count(id.Name()))
			ReportForwardRef(env, id.Name(), *id.Location());
		return;   //an identifier leaf owns no children to scan
	}
	for (auto& child : node.Children())
		ScanForForwardRefs(child, className, later, env);
}

void CheckStaticForwardRefsIn(SnClassDecl& decl, BuildEnvironment& env)
{
	std::vector<SnClassField*> statics;
	for (auto& member : decl.Members())
		if (member.Kind() == NK_ClassField
			&& member.ContainFlags(NF_Static))
			statics.push_back(&static_cast<SnClassField&>(member));
	for (size_t i = 0; i < statics.size(); ++i) {
		SnExpression* init = statics[i]->Initializer();
		if (!init)
			continue;
		std::unordered_set<std::string> later;
		for (size_t j = i + 1; j < statics.size(); ++j)
			later.insert(statics[j]->Name());
		if (!later.empty())
			ScanForForwardRefs(*init, decl.Name(), later, env);
	}
}

} //namespace

//D12 layer 1 pre-pass entry (declaration in StatementResolver.h's public
//pre-pass section): member function so the free helpers reach m_Env.
void StatementResolveAccessor::CheckStaticForwardRefs(SnClassDecl& sn)
{
	CheckStaticForwardRefsIn(sn, m_Env);
}

void StatementResolver::PreSynthesizeStaticInitializers(Node& node,
	StatementResolveAccessor& accessor)
{
	//All classes must be base-linked before the chain query, so this
	//runs after PreResolveClassBases and computes need in one pass over
	//a shared memo. D12 layer 1 rides the same walk: forward-reference
	//checking needs only the declaration order and the raw initializer
	//expressions, both intact before synthesis detaches them.
	std::unordered_map<SnClassDecl*, bool> memo;
	std::vector<SnClassDecl*> work;
	std::function<void(Node&)> collect = [&](Node& n) {
		if (n.Kind() == NK_ClassDecl) {
			auto& decl = static_cast<SnClassDecl&>(n);
			//Imported stubs carry no initializer expressions (their
			//producer owns the initializer; cross-module lazy touch is
			//the import task's scope). BuildArtifacts resolves each
			//tree exactly once, so no re-synthesis guard is needed.
			if (!decl.IsImported()) {
				accessor.CheckStaticForwardRefs(decl);
				if (ClassNeedsStaticInit(decl, memo))
					work.push_back(&decl);
			}
		}
		for (auto& child : n.Children())
			collect(child);
	};
	collect(node);
	for (auto* decl : work)
		SynthesizeOneClass(*decl);
}

} //namespace nlang
