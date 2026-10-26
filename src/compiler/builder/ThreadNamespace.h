/*-----------------------------------------------------------------------------
	ncomp/builder/ThreadNamespace.h
	K3 (v0.10.3): the "thread" package materialized as an ordinary stdlib
	native package (stdlib/thread.n carries the declarations; the
	implementations live inside the VM). This header is what remains of
	the K1 built-in-namespace machinery after the materialization:
	① the package name (shared by the resolver's typed-spawn arm and
	package admission); the backend's intrinsic stamping moved to the
	general binding table (VmBackend's IsStampedBindingPackage /
	LookupIntrinsicId);
	② the Mutex method-shape struct for the sync-primitive resolver arm
	(ExprResolverThreadNs.cpp).
-----------------------------------------------------------------------------*/
#pragma once
#include <cstdint>
#include <string>
#include <nlang/runtime/NodeConsts.h>

namespace nlang
{

//The standard library's concurrency-primitives package name. The package
//declares itself in stdlib/thread.n; a user unit under the same path
//collides with it through the ordinary duplicate-package rule.
inline constexpr const char* kThreadNamespaceName = "thread";

//Fixed call shape of one Mutex/Channel method (the resolver side's
//classification result; Channel.recv arrives in Slice 9 with its
//element-type decision — until then it is simply not a member).
struct SyncMethodShape {
	int paramCount;
	NodeKind returnType;   //NK_Void = no result value on the member
};

} //namespace nlang
