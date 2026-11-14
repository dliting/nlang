/*---
GenericMintDepthClaim.hpp — the RAII claim around a generic mint's
recursive window, shared by the class and interface mint TUs.

Scoped increment of the concurrent-mint depth around the mint window:
nested instantiations fire synchronously inside BOTH phases — template
completion resolves the template's own member types (a member type
like A<A<T>> resolves through Access back into the mint), and member
cloning substitutes them into the instance — each nested mint through
a freshly constructed resolver, so the chain counter lives on the
registry, shared across resolver instances, and the guard reads it at
the mint entry.
---*/
#pragma once
#include <nlang/compiler/GenericInstanceRegistry.h>

namespace nlang
{

class MintDepthClaim
{
public:
	explicit MintDepthClaim(GenericInstanceRegistry& registry)
		: m_rRegistry(registry)
		{ m_rRegistry.EnterMintDepth(); }
	~MintDepthClaim() { m_rRegistry.LeaveMintDepth(); }
	MintDepthClaim(const MintDepthClaim&) = delete;
	MintDepthClaim& operator=(const MintDepthClaim&) = delete;
private:
	GenericInstanceRegistry& m_rRegistry;
};

} //namespace nlang
