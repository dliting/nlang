/*-----------------------------------------------------------------------------
	ncomp/builder/ThreadNamespace.h
	K3 (v0.10.3): the "thread" package materialized as an ordinary stdlib
	native package (stdlib/thread.n carries the declarations; the
	implementations live inside the VM). This header is what remains of
	the K1 built-in-namespace machinery after the materialization:
	① the package name (shared by the resolver's typed-spawn arm and the
	backend's intrinsic stamping);
	② the name → intrinsic id table the backend stamps thread.n's native
	records with (nlang_vm links nlang_compiler privately and includes
	builder headers, the same arrangement SymbolSlots.hpp uses);
	③ the Mutex method-shape struct for the sync-primitive resolver arm
	(ExprResolverThreadNs.cpp).
-----------------------------------------------------------------------------*/
#pragma once
#include <cstdint>
#include <string>
#include <nlang/runtime/NodeConsts.h>
#include <nlang/vm/Intrinsics.h>

namespace nlang
{

//The standard library's concurrency-primitives package name. The package
//declares itself in stdlib/thread.n; a user unit under the same path
//collides with it through the ordinary duplicate-package rule.
inline constexpr const char* kThreadNamespaceName = "thread";

//The five VM-implemented members of the thread package: stdlib/thread.n
//declares them `native`, and the backend flips each record onto the
//intrinsic channel (isNative=false + intrinsicId) because the
//implementations are built into the VM, not a host library.
struct ThreadIntrinsicEntry {
	const char* memberName;
	uint16_t intrinsicId;
};

//INTR_None when the name is not a VM-implemented thread member (such a
//declaration keeps host-native dispatch — the ordinary native channel).
uint16_t FindThreadIntrinsicId(const std::string& memberName);

//Fixed call shape of one Mutex/Channel method (the resolver side's
//classification result; Channel.recv arrives in Slice 9 with its
//element-type decision — until then it is simply not a member).
struct SyncMethodShape {
	int paramCount;
	NodeKind returnType;   //NK_Void = no result value on the member
};

} //namespace nlang
