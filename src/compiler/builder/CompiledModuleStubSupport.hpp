#pragma once
#include "SnExpressions.h"
#include "SyntaxTree.h"
#include "nlang/vm/CompiledModule.h"
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

} //namespace nlang
