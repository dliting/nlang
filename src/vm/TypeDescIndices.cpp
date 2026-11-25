/*---
TypeDescIndices.cpp — consumer-side index handling for the recursive type
descriptors declared in TypeDesc.h: the post-load bounds check against the
module's declaration tables, and the import-merge rewrite of table indices
from producer to consumer numbering. The descriptor grammar itself
(append / parse / build) stays in TypeDesc.cpp.
---*/
#include "nlang/core/TypeDesc.h"
//RTK_* kind constants (the same family the field-type kind bytes use).
#include "nlang/core/CompiledModule.h"
#include <stdexcept>

namespace nlang {

void ValidateTypeDescIndices(const TypeDesc& td, size_t structCount,
	size_t classCount, size_t interfaceCount)
{
	if (td.kind == RTK_Struct)
	{
		if (td.typeIdx >= structCount)
			throw std::runtime_error(
				"Invalid module: type descriptor struct index out of range");
	}
	else if (td.kind == RTK_Class)
	{
		if (td.typeIdx >= classCount)
			throw std::runtime_error(
				"Invalid module: type descriptor class index out of range");
	}
	//v0.13.9 Task 12 (D14): the identity tier's row names the interfaces
	//table — validate against it (defaulted for host-built modules with
	//no interfaces table).
	else if (td.kind == RTK_Interface && interfaceCount != kNoInterfaceCount)
	{
		if (td.typeIdx >= interfaceCount)
			throw std::runtime_error(
				"Invalid module: type descriptor interface index out of range");
	}
	for (const auto& elem : td.elems)
		ValidateTypeDescIndices(elem, structCount, classCount,
			interfaceCount);
}

void RemapTypeDesc(TypeDesc& td,
	const std::unordered_map<uint32_t, uint32_t>& structMap,
	const std::unordered_map<uint32_t, uint32_t>& classMap,
	const std::unordered_map<uint32_t, uint32_t>& interfaceMap)
{
	//The merge maps are total over each kind's producer indices (see the
	//note in TypeDesc.h) — a miss is an upstream construction bug, so
	//.at() throws instead of silently keeping a stale index.
	if (td.kind == RTK_Struct)
		td.typeIdx = static_cast<uint16_t>(structMap.at(td.typeIdx));
	else if (td.kind == RTK_Class)
		td.typeIdx = static_cast<uint16_t>(classMap.at(td.typeIdx));
	//v0.13.9 Task 12 (D14): the identity tier remaps through the
	//interfaces merge map when the caller provides one; an RTK_Interface
	//leaf with no map is a caller bug only when the unit declared one —
	//units without interface tables pass the empty default.
	else if (td.kind == RTK_Interface && !interfaceMap.empty())
		td.typeIdx = static_cast<uint16_t>(interfaceMap.at(td.typeIdx));
	for (auto& elem : td.elems)
		RemapTypeDesc(elem, structMap, classMap, interfaceMap);
}

} //namespace nlang
