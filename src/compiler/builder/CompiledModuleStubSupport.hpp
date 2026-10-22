#pragma once
#include "SnExpressions.h"
#include "SyntaxTree.h"
#include "nlang/vm/CompiledModule.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <memory>
#include <string>
#include <vector>

namespace nlang
{

//Source location for stubs constructed from a CompiledModule.
//Distinct from ImportedNodeLocation (which is backed by a RuntimeNode);
//here we have no RuntimeNode — the stub is minted directly from .ncu data.
class CompiledModuleNodeLocation : public ISourceLocation
{
public:
	explicit CompiledModuleNodeLocation(const std::string& moduleName) :
		m_moduleName(moduleName)
	{
	}

	~CompiledModuleNodeLocation() override
	{
	}

	std::unique_ptr<ISourceLocation> Clone() const override
	{
		return std::make_unique<CompiledModuleNodeLocation>(m_moduleName);
	}

	std::string ToString() const override
	{
		return "imported from module '" + m_moduleName + "'";
	}

	TranslationUnit *TransUnit() const override
	{
		return nullptr;
	}
private:
	std::string m_moduleName;
};

//LeafNameOfKey lives in nlang/vm/CompiledModule.h (shared with the VM
//render sites).

//Synthesize the type expression for a serialized table key. A dotted key
//('c.S', 'lib.Handler') must take the qualified type path — the same node
//hand-written source produces for `import c; c.S s;` — so Access
//(SnQualifiedTypeExpr) binds it to the owning module's stub through the
//registry. A bare-name identifier would resolve against the consumer's
//own root and could silently bind a same-named local type instead.
inline SnFieldExpr *NamedTypeExprFromKey(const std::string &key,
	const ISourceLocation &loc)
{
	std::vector<std::string> segs;
	size_t start = 0;
	for (;;)
	{
		const size_t dot = key.find('.', start);
		segs.emplace_back(key, start,
			(dot == std::string::npos ? key.size() : dot) - start);
		if (dot == std::string::npos)
			break;
		start = dot + 1;
	}
	if (segs.size() == 1)
		return new SnIdentifierExpr(new std::string(key), loc);
	auto *pQType = new SnQualifiedTypeExpr(segs[0], segs[1], loc);
	for (size_t i = 2; i < segs.size(); ++i)
		pQType->AppendSegment(segs[i]);
	return pQType;
}

//v1.12: synthesize a SYNTACTIC type expression from a serialized type
//descriptor — the same shape the parser would build for hand-written
//source, so the consumer's normal ResolveDataTypes pass binds and
//interns it through the standard channels (named types resolve against
//the root, where the stub builder placed the imported struct/class
//stubs; the List/Dict/func base names go through the built-in generic
//instantiation path; builtin scalar kinds arrive pre-bound via the
//NodeKind ctor). NonSerialized (interface types, defensive misses)
//degrades to the int32 placeholder. Free function since 0.9.10 (moved
//verbatim out of CompiledModuleNodeBuilder for the source-size cap);
//the stub builder and both stub-member helpers share it.
inline SnFieldExpr *SynthTypeExprFromDesc(const TypeDesc &td,
	const CompiledModule &cm, const ISourceLocation &loc)
{
	//0.7.5: scalar kinds (legacy RTK_Int32/RTK_Float and the
	//RTK_Byte..RTK_Char family) resolve through the registry — the
	//old switch handled only RTK_Float, silently degrading every
	//other scalar to the int32 placeholder (an imported `long`
	//formal/return truncated its 8-byte values at consumer call
	//sites before this arm existed).
	if (int pi = ScalarPrimIndexOfRtk(td.kind); pi >= 0)
		return new SnIdentifierExpr(kScalarPrims[pi].kind, loc);
	switch (td.kind)
	{
		case RTK_String:
			return new SnIdentifierExpr(NK_String, loc);
		case RTK_Struct:
			if (td.typeIdx < cm.structs.size())
				return NamedTypeExprFromKey(
					cm.structs[td.typeIdx].name, loc);
			break;  //out-of-range index — int32 placeholder below
		case RTK_Class:
			if (td.typeIdx < cm.classes.size())
				return NamedTypeExprFromKey(
					cm.classes[td.typeIdx].name, loc);
			break;
		case RTK_Array:
			if (!td.elems.empty())
				return new SnArrayTypeExpr(
					SynthTypeExprFromDesc(td.elems[0], cm, loc), loc);
			break;
		case RTK_List:
			if (!td.elems.empty())
			{
				auto *pArgs = new std::vector<SnFieldExpr*>();
				pArgs->push_back(SynthTypeExprFromDesc(td.elems[0], cm, loc));
				return new SnGenericTypeExpr(
					new SnIdentifierExpr(
						new std::string(kBuiltinListTypeName), loc),
					pArgs, loc);
			}
			break;
		case RTK_Dict:
			if (td.elems.size() >= 2)
			{
				auto *pArgs = new std::vector<SnFieldExpr*>();
				pArgs->push_back(SynthTypeExprFromDesc(td.elems[0], cm, loc));
				pArgs->push_back(SynthTypeExprFromDesc(td.elems[1], cm, loc));
				return new SnGenericTypeExpr(
					new SnIdentifierExpr(
						new std::string(kBuiltinDictTypeName), loc),
					pArgs, loc);
			}
			break;
		case RTK_Func:
			//v2.2: wire order is params..., return — the func<R, P...>
			//spelling takes the return first. The generic-expression
			//shape routes through the resolver's synthetic-decl cache
			//(GetGenericClassDecl), so the rebuilt type is pointer-
			//identical to the consumer's own func<...> syntax — the
			//identity FuncRefMatchesDecl's argument check relies on.
			if (!td.elems.empty())
			{
				auto *pArgs = new std::vector<SnFieldExpr*>();
				//Void serializes as the RTK_Void leaf; rebuild the
				//same NK_Void node the parser mints for func<void,...>.
				pArgs->push_back(td.elems.back().kind == RTK_Void
					? static_cast<SnFieldExpr*>(
						new SnIdentifierExpr(NK_Void, loc))
					: SynthTypeExprFromDesc(td.elems.back(), cm, loc));
				for (size_t i = 0; i + 1 < td.elems.size(); ++i)
					pArgs->push_back(
						SynthTypeExprFromDesc(td.elems[i], cm, loc));
				return new SnGenericTypeExpr(
					new SnIdentifierExpr(
						new std::string(kBuiltinFuncTypeName), loc),
					pArgs, loc);
			}
			break;
		default:
			break;
	}
	return new SnIdentifierExpr(NK_Int32, loc);
}

} //namespace nlang
