/*---
	BindingTable.h — the intrinsic binding table: which declared
	members of which packages are native-implemented, and at which
	intrinsic channel. Pure compile-time declaration metadata — the
	runtime dispatches on the intrinsic id baked into the record and
	never reads the table. Lives with the compiler (concurrency
	import redesign): the resolver consults its projections (D10/D11)
	from pure-compiler link targets, and the sanctioned link
	direction is vm -> compiler.
---*/
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nlang
{

class ModuleRegistry;

//One VM-implemented declaration by owning package + key. noReceiver
//marks the static-face ABI: no `this` rides slot 0 — the intrinsic's
//arguments start at slot 0 (every receiver-dispatched row defaults to
//false: slot 0 = the instance).
struct BindingRow
{
	std::string packagePath;
	std::string key;
	uint16_t intrinsicId;
	bool noReceiver = false;
};

//The declared rows merged with the derived static-face rows.
const std::vector<BindingRow>& BindingRows();

//True when the package owns at least one binding row (the two-arm
//stamping gate's package test).
bool IsStampedBindingPackage(const std::string& packagePath);

//Row lookup: the intrinsic id for a package + key, INTR_None when the
//declaration is not table-covered (host-native dispatch).
uint16_t LookupIntrinsicId(const std::string& packagePath,
	const std::string& key);

//D10 (concurrency import redesign): reverse lookup for the unimported-
//use diagnostic — which stamped package declares this bare name? Four
//key forms: exact ctor, "/1" ctor, "Name." method prefix, and the
//free-function "pkg.name" qualified form. outIsFreeFuncForm (optional)
//reports whether the match came from that last form — the note for a
//package function must name the qualified spelling, not just the
//import (package functions stay qualified-only even when imported).
bool BindingPackageForName(const std::string& name,
	std::string& outPackage, bool* outIsFreeFuncForm = nullptr);

//Handle-class marker (§6.4): true for a prelude class name whose
//record carries the hidden __handle field. Append-only list.
bool IsHandleClassName(const std::string& className);

//D11 (concurrency import redesign): the successor "this declaration IS
//the builtin one" predicate — prelude ownership, or a stamped binding
//package owning a handle-class name. Thread/Future have no class
//binding rows, so this is a name-list arm, not a row-presence test.
bool IsBuiltinHandleDeclName(const ModuleRegistry& registry,
	uint32_t ownerUnit, const std::string& className);

} //namespace nlang
