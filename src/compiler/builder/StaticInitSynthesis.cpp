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

} //namespace

void StatementResolver::PreSynthesizeStaticInitializers(Node& node,
	StatementResolveAccessor& accessor)
{
	(void)accessor;
	//All classes must be base-linked before the chain query, so this
	//runs after PreResolveClassBases and computes need in one pass over
	//a shared memo.
	std::unordered_map<SnClassDecl*, bool> memo;
	std::vector<SnClassDecl*> work;
	std::function<void(Node&)> collect = [&](Node& n) {
		if (n.Kind() == NK_ClassDecl) {
			auto& decl = static_cast<SnClassDecl&>(n);
			//Imported stubs carry no initializer expressions (their
			//producer owns the initializer; cross-module lazy touch is
			//the import task's scope). BuildArtifacts resolves each
			//tree exactly once, so no re-synthesis guard is needed.
			if (!decl.IsImported() && ClassNeedsStaticInit(decl, memo))
				work.push_back(&decl);
		}
		for (auto& child : n.Children())
			collect(child);
	};
	collect(node);
	for (auto* decl : work)
		SynthesizeOneClass(*decl);
}

} //namespace nlang
