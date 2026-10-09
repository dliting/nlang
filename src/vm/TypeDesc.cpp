/*---
TypeDesc.cpp — .ncu v1.12 type descriptors (build / serialize / parse).

One grammar shared by the writer (VmBackend captures, ModuleSaver emits)
and the reader (ModuleLoader parses + validates). Keep AppendTypeDescBytes
and ParseOne in lockstep with the struct comment in TypeDesc.h.
---*/
#include "nlang/vm/TypeDesc.h"
//RTK_* kind constants (also the TypeDesc field-width contract).
#include "nlang/vm/CompiledModule.h"
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/compiler/SnMisc.h>
#include <stdexcept>

namespace nlang {

void AppendTypeDescBytes(std::vector<uint8_t>& out, const TypeDesc& td)
{
	out.push_back(td.kind);
	if (td.kind == RTK_Struct || td.kind == RTK_Class)
	{
		out.push_back(static_cast<uint8_t>(td.typeIdx & 0xFF));
		out.push_back(static_cast<uint8_t>(td.typeIdx >> 8));
	}
	else if (td.kind == RTK_Array || td.kind == RTK_List)
	{
		if (td.elems.empty())
			throw std::runtime_error(
				"type descriptor arity violation: container kind without "
				"its nested descriptor");
		AppendTypeDescBytes(out, td.elems[0]);
	}
	else if (td.kind == RTK_Dict)
	{
		if (td.elems.size() < 2)
			throw std::runtime_error(
				"type descriptor arity violation: Dict without K and V "
				"descriptors");
		AppendTypeDescBytes(out, td.elems[0]);
		AppendTypeDescBytes(out, td.elems[1]);
	}
}

namespace {

TypeDesc ParseOne(const uint8_t* pData, size_t size, size_t& pos,
	size_t depth)
{
	if (depth > kMaxTypeDescDepth)
		throw std::runtime_error("Invalid module: type descriptor too deep");
	if (pos >= size)
		throw std::runtime_error("Invalid module: truncated type descriptor");
	TypeDesc td;
	td.kind = pData[pos++];
	switch (td.kind)
	{
		case RTK_Struct:
		case RTK_Class:
			if (size - pos < 2)
				throw std::runtime_error(
					"Invalid module: truncated type descriptor");
			td.typeIdx = static_cast<uint16_t>(pData[pos])
				| (static_cast<uint16_t>(pData[pos + 1]) << 8);
			pos += 2;
			break;
		case RTK_Array:
		case RTK_List:
			td.elems.push_back(ParseOne(pData, size, pos, depth + 1));
			break;
		case RTK_Dict:
			td.elems.push_back(ParseOne(pData, size, pos, depth + 1));
			td.elems.push_back(ParseOne(pData, size, pos, depth + 1));
			break;
		case RTK_String:
		case RTK_Boxed:
		case RTK_Func:
		case RTK_NonSerialized:
			break;
		default:
			//0.7.5: every scalar kind (RTK_Int32/RTK_Float and the
			//RTK_Byte..RTK_Char family) is a kind-byte-only leaf —
			//accept through the registry instead of enumerating rows.
			if (ScalarPrimIndexOfRtk(td.kind) < 0)
				throw std::runtime_error(
					"Invalid module: unknown type descriptor kind");
			break;
	}
	return td;
}

} //namespace

TypeDesc ParseTypeDescBytes(const uint8_t* pData, size_t size)
{
	size_t pos = 0;
	TypeDesc td = ParseOne(pData, size, pos, 0);
	if (pos != size)
		throw std::runtime_error(
			"Invalid module: type descriptor trailing bytes");
	return td;
}

void ValidateTypeDescIndices(const TypeDesc& td, size_t structCount,
	size_t classCount)
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
	for (const auto& elem : td.elems)
		ValidateTypeDescIndices(elem, structCount, classCount);
}

void RemapTypeDesc(TypeDesc& td,
	const std::unordered_map<uint32_t, uint32_t>& structMap,
	const std::unordered_map<uint32_t, uint32_t>& classMap)
{
	//The merge maps are total over each kind's producer indices (see the
	//note in TypeDesc.h) — a miss is an upstream construction bug, so
	//.at() throws instead of silently keeping a stale index.
	if (td.kind == RTK_Struct)
		td.typeIdx = static_cast<uint16_t>(structMap.at(td.typeIdx));
	else if (td.kind == RTK_Class)
		td.typeIdx = static_cast<uint16_t>(classMap.at(td.typeIdx));
	for (auto& elem : td.elems)
		RemapTypeDesc(elem, structMap, classMap);
}

TypeDesc BuildTypeDesc(SnField* pType, const TypeLeafSlots& slots,
	size_t depth)
{
	TypeDesc td;
	if (!pType)
	{
		td.kind = RTK_NonSerialized;
		return td;
	}
	//Dispatch discipline (EvalDataType trap): array-ness first — an
	//interned token reports IsArrayType() while Kind() is the token's
	//own kind, so a Kind()-first switch would miss it (RuntimeTypeKind
	//keeps the same order).
	if (pType->IsArrayType())
	{
		if (pType->Kind() != NK_ArrayTypeToken)
		{
			//Not an interned token: an unresolved shape whose element is
			//not recoverable — degrade the whole descriptor.
			td.kind = RTK_NonSerialized;
			return td;
		}
		//Writer side of kMaxTypeDescDepth: a container at the cap would
		//nest its element one past it, and the loader would reject the
		//module ncc just wrote — degrade instead (same rule at the
		//List/Dict arms below).
		if (depth >= kMaxTypeDescDepth)
		{
			td.kind = RTK_NonSerialized;
			return td;
		}
		td.kind = RTK_Array;
		td.elems.push_back(BuildTypeDesc(
			static_cast<SnArrayTypeToken*>(pType)->ElemTypeOf(), slots,
			depth + 1));
		return td;
	}
	switch (pType->Kind())
	{
		case NK_EnumDecl:
			//Enums are int32 at runtime; CompiledModule has no enum name
			//table entries for parameter types, so no index is carried.
			td.kind = RTK_Int32;
			return td;
		case NK_StructDecl:
		{
			td.kind = RTK_Struct;
			td.typeIdx = static_cast<uint16_t>(
				slots.structSlotOf(*pType));
			return td;
		}
		case NK_InterfaceDecl:
			//CompiledModule has no interfaces table — degrade.
			td.kind = RTK_NonSerialized;
			return td;
		case NK_ClassDecl:
		{
			auto* pClass = static_cast<SnClassDecl*>(pType);
			if (pClass->IsFuncType())
			{
				//Func signatures are outside the v1.12 descriptor grammar
				//— consumers keep the placeholder rejection.
				td.kind = RTK_NonSerialized;
				return td;
			}
			if (pClass->IsGenericInstantiation())
			{
				//Bare BaseName comparisons: builtin erasure keys — the
				//instantiation node is ownerless and its backing class is
				//registered once under "List"/"Dict".
				const auto& args = pClass->GenericTypeArgs();
				if (pClass->BaseName() == kBuiltinListTypeName && args.size() == 1)
				{
					if (depth >= kMaxTypeDescDepth)
					{
						td.kind = RTK_NonSerialized;
						return td;
					}
					td.kind = RTK_List;
					td.elems.push_back(BuildTypeDesc(args[0], slots, depth + 1));
					return td;
				}
				if (pClass->BaseName() == kBuiltinDictTypeName && args.size() == 2)
				{
					if (depth >= kMaxTypeDescDepth)
					{
						td.kind = RTK_NonSerialized;
						return td;
					}
					td.kind = RTK_Dict;
					td.elems.push_back(BuildTypeDesc(args[0], slots, depth + 1));
					td.elems.push_back(BuildTypeDesc(args[1], slots, depth + 1));
					return td;
				}
				//Any other instantiation is unexpected (Func took the
				//exit above) — fall through to the plain-class path.
			}
			td.kind = RTK_Class;
			td.typeIdx = static_cast<uint16_t>(
				slots.classSlotOf(*pClass));
			return td;
		}
		default:
		{
			//Builtin leaves: NK_* and RTK_* are independent numberings
			//since the basic-types expansion (the old layout held a
			//coincidental identity for int/float/string) — map through the
			//registry, never a cast. String is the one non-scalar builtin.
			//0.7.5: every registry scalar emits its RTK (the pre-0.7.5 arm
			//whitelisted only int32/float, degrading imported long
			//formals/returns to NonSerialized — the consumer's int32
			//placeholder then truncated the 8-byte values).
			if (pType->Kind() == NK_String)
			{
				td.kind = RTK_String;
				return td;
			}
			const uint8_t rtk = RtkOfKind(pType->Kind());
			td.kind = (rtk != 0xFF) ? rtk : RTK_NonSerialized;
			return td;
		}
	}
	td.kind = RTK_NonSerialized;   //defensive tail: every arm returns
	return td;
}

} //namespace nlang
