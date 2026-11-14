/*---
GenericInstantiationDepthClaim.hpp — the RAII claim around a generic creation's
recursive window, shared by the class and interface creation TUs.

Scoped increment of the concurrent-instantiation depth around the creation window:
nested instantiations fire synchronously inside BOTH phases — template
completion resolves the template's own member types (a member type
like A<A<T>> resolves through Access back into the creation), and member
cloning substitutes them into the instance — each nested creation through
a freshly constructed resolver, so the chain counter lives on the
registry, shared across resolver instances, and the guard reads it at
the creation entry.
---*/
#pragma once
#include <nlang/compiler/GenericInstanceRegistry.h>

namespace nlang
{

class InstantiationDepthClaim
{
public:
	explicit InstantiationDepthClaim(GenericInstanceRegistry& registry)
		: m_rRegistry(registry)
		{ m_rRegistry.EnterInstantiationDepth(); }
	~InstantiationDepthClaim() { m_rRegistry.LeaveInstantiationDepth(); }
	InstantiationDepthClaim(const InstantiationDepthClaim&) = delete;
	InstantiationDepthClaim& operator=(const InstantiationDepthClaim&) = delete;
private:
	GenericInstanceRegistry& m_rRegistry;
};

} //namespace nlang
