/*-----------------------------------------------------------------------------
	ncomp/builder/ThreadNamespace.h
	K3 (v0.10.3): the "thread" package materialized as an ordinary stdlib
	native package (stdlib/thread.n carries the declarations; the
	implementations live inside the VM). This header is what remains of
	the K1 built-in-namespace machinery: the package name (shared by the
	resolver's typed-spawn arm and package admission). The backend's
	intrinsic stamping moved to the general binding table (VmBackend's
	IsStampedBindingPackage / LookupIntrinsicId); the sync-primitive
	resolver arm retired with the builtin-decls migration — Mutex/
	Semaphore methods resolve as ordinary declared members now.
-----------------------------------------------------------------------------*/
#pragma once
#include <cstdint>
#include <string>

namespace nlang
{

//The standard library's concurrency-primitives package name. The package
//declares itself in stdlib/thread.n; a user unit under the same path
//collides with it through the ordinary duplicate-package rule.
inline constexpr const char* kThreadNamespaceName = "thread";

} //namespace nlang
