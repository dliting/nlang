/*---
TypeDesc.cpp — .nmod v1.12 type descriptors (build / serialize / parse).

One grammar shared by the writer (VmBackend captures, ModuleSaver emits)
and the reader (ModuleLoader parses + validates). Keep AppendTypeDescBytes
and ParseOne in lockstep with the struct comment in TypeDesc.h.
---*/
#include "nlang/vm/TypeDesc.h"
#include "nlang/vm/CompiledModule.h"
#include <nlang/compiler/SnArrayTypeToken.h>
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
		case RTK_Int32:
		case RTK_Float:
		case RTK_String:
		case RTK_Boxed:
		case RTK_Func:
		case RTK_NonSerialized:
			break;
		default:
			throw std::runtime_error(
				"Invalid module: unknown type descriptor kind");
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

TypeDesc BuildTypeDesc(const SnField* pType, const CompiledModule& mod,
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
			static_cast<const SnArrayTypeToken*>(pType)->ElemTypeOf(), mod,
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
			int idx = mod.FindStruct(pType->Name());
			if (idx < 0)
				break;  //not registered — degrade
			td.kind = RTK_Struct;
			td.typeIdx = static_cast<uint16_t>(idx);
			return td;
		}
		case NK_InterfaceDecl:
			//CompiledModule has no interfaces table — degrade.
			td.kind = RTK_NonSerialized;
			return td;
		case NK_ClassDecl:
		{
			auto* pClass = static_cast<const SnClassDecl*>(pType);
			if (pClass->IsFuncType())
			{
				//Func signatures are outside the v1.12 descriptor grammar
				//— consumers keep the placeholder rejection.
				td.kind = RTK_NonSerialized;
				return td;
			}
			if (pClass->IsGenericInstantiation())
			{
				const auto& args = pClass->GenericTypeArgs();
				if (pClass->BaseName() == "List" && args.size() == 1)
				{
					if (depth >= kMaxTypeDescDepth)
					{
						td.kind = RTK_NonSerialized;
						return td;
					}
					td.kind = RTK_List;
					td.elems.push_back(BuildTypeDesc(args[0], mod, depth + 1));
					return td;
				}
				if (pClass->BaseName() == "Dict" && args.size() == 2)
				{
					if (depth >= kMaxTypeDescDepth)
					{
						td.kind = RTK_NonSerialized;
						return td;
					}
					td.kind = RTK_Dict;
					td.elems.push_back(BuildTypeDesc(args[0], mod, depth + 1));
					td.elems.push_back(BuildTypeDesc(args[1], mod, depth + 1));
					return td;
				}
				//Any other instantiation is unexpected (Func took the
				//exit above) — fall through to the plain-class path.
			}
			int idx = mod.FindClass(pClass->Name());
			if (idx < 0)
				break;  //not registered — degrade
			td.kind = RTK_Class;
			td.typeIdx = static_cast<uint16_t>(idx);
			return td;
		}
		default:
		{
			//Scalar builtin leaves: the Int32/Float/String NK_* values fit
			//the low RTK bytes (the same cast RuntimeTypeKind relies on).
			//The RTK_Boxed/RTK_Func bytes collide with NK kinds that are
			//never type nodes (FormalParam/Namespace), so no node can
			//legitimately produce them — degrade anything else instead of
			//emitting a byte the parser rejects.
			uint8_t k = static_cast<uint8_t>(pType->Kind());
			if (k == RTK_Int32 || k == RTK_Float || k == RTK_String)
				td.kind = k;
			else
				td.kind = RTK_NonSerialized;
			return td;
		}
	}
	td.kind = RTK_NonSerialized;
	return td;
}

} //namespace nlang
