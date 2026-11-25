/*---
TypeDescBuild.cpp — BuildTypeDesc: the writer-side AST node ->
wire type descriptor map, split out of TypeDesc.cpp for the
library layering (docs/dev/library_layering_design.md §6); this TU
joins the compiler side when the backend moves there in task 5.
The wire grammar itself (serialize / parse) stays in TypeDesc.cpp
— keep the two in lockstep with the struct comment in TypeDesc.h.
---*/
#include "nlang/core/TypeDesc.h"
//RTK_* kind constants (also the TypeDesc field-width contract).
#include "nlang/core/CompiledModule.h"
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/StackDump.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SyntaxNodeConsts.h>
#include <stdexcept>
#include <string>

namespace nlang {

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
	//keeps the same order). v0.13.7: the class-row spelling of the same
	//array type (Array<E>, the generic instantiation) is array-typed
	//too, but it stays OUT of this arm — its designed route is the
	//class-decl arm's erasure below (RTK_Array rank 0, unannotated);
	//this arm writes the bracket spelling's static rank instead.
	if (pType->IsArrayType() && pType->Kind() == NK_ArrayTypeToken)
	{
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
		td.rank = static_cast<SnArrayTypeToken*>(pType)->Rank();
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
			//v0.13.9 Task 12 (D14): the identity tier — name the
			//interface itself (RTK_Interface + interfaces-table row) so
			//static-slot roots, cast-row keys and linker merges keep
			//interface identity. The pre-fix degrade to
			//RTK_NonSerialized fell out of every GC root routing set
			//and the constant's instance was swept mid-churn.
			td.kind = RTK_Interface;
			td.typeIdx = static_cast<uint16_t>(
				slots.interfaceSlotOf(*pType));
			return td;
		case NK_ClassDecl:
		{
			auto* pClass = static_cast<SnClassDecl*>(pType);
			if (pClass->IsFuncType())
			{
				//v2.2: func signatures serialize — the synthetic class
				//carries the signature as its generic type args
				//(args[0] = return, args[1..] = params). Wire order is
				//params first, return last. Depth cap mirrors the
				//container arms: a signature nested at the cap would
				//write children one past it and the loader would reject
				//the module ncc just wrote.
				if (depth >= kMaxTypeDescDepth)
				{
					td.kind = RTK_NonSerialized;
					return td;
				}
				const auto &args = pClass->GenericTypeArgs();
				if (args.empty())
				{
					//Defensive: GetGenericClassDecl enforces arity >= 1,
					//so a func decl without type args cannot come from
					//the resolver.
					td.kind = RTK_NonSerialized;
					return td;
				}
				td.kind = RTK_Func;
				//v2.21: the reference mark rides the instantiation's
				//flag vector (&, indexed by argument position—0 =
				//return). The wire element order is params first,
				//return last, so the vector is read position-flipped
				//to stay parallel to elems.
				const auto &refFlags = pClass->GenericRefFlags();
				const auto qualifierOf = [&refFlags](size_t argIdx)
				{
					uint8_t bits = 0;
					if (argIdx < refFlags.size() && refFlags[argIdx])
						bits |= PTDF_Ref;
					return bits;
				};
				for (size_t i = 1; i < args.size(); ++i)
				{
					td.elems.push_back(
						BuildTypeDesc(args[i], slots, depth + 1));
					td.elemFlags.push_back(qualifierOf(i));
				}
				//Void is expressible only as the func return slot —
				//the grammar has no other void leaf position.
				td.elems.push_back(
					args[0]->Kind() == NK_Void
						? TypeDesc{RTK_Void}
						: BuildTypeDesc(args[0], slots, depth + 1));
				td.elemFlags.push_back(qualifierOf(0));
				return td;
			}
			if (pClass->IsTypeParamPlaceholder())
			{
				//v0.12.3 dictionary model: a template signature's
				//parameterized leaf — one value slot whatever the
				//instantiation binds (the unified 8-byte value
				//representation). The runtime type argument descriptors
				//carry the per-instantiation representation; the wire
				//kind is the label itself. v0.13.1 D7: the declared
				//position rides the leaf so the consumer stub names it.
				td.kind = RTK_TypeParam;
				td.typeParamOrdinal = slots.typeParamOrdinalOf
					? slots.typeParamOrdinalOf(*pClass)
					: 0xFF;
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
				//v0.12.7: Array<E> erases to the array record kind with
				//rank 0 (unannotated) — the bracket spelling writes
				//1..kMaxArrayRank via the array-token arm above.
				if (pClass->BaseName() == kBuiltinArrayTypeName
					&& args.size() == 1)
				{
					if (depth >= kMaxTypeDescDepth)
					{
						td.kind = RTK_NonSerialized;
						return td;
					}
					td.kind = RTK_Array;
					td.rank = 0;
					td.elems.push_back(BuildTypeDesc(args[0], slots, depth + 1));
					return td;
				}
				//Any other instantiation is unexpected (func took the
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
			if (rtk != 0xFF)
			{
				td.kind = rtk;
				return td;
			}
			//The other NonSerialized exits above are designed degradations
			//(no interfaces table, the nesting cap). This one is not: it is a
			//kind with no wire code at all. Every type shape a resolved
			//declaration can name has an arm here — the builtin leaves, the
			//four declaration kinds, the interned array token, func handles,
			//type parameters and the three builtin container erasures — so
			//landing here means a declaration kind was added without its
			//descriptor route. The silent degrade wrote a descriptor the
			//consumer reads as "unknown type", which is how such an omission
			//hid until a boxing or GC decision read the wrong kind.
			NLANG_ASSERT(false && "type has no wire type kind");
			throw std::logic_error(
				std::string("no type descriptor for node kind ")
					+ NodeKindDebugName(pType->Kind()));
		}
	}
	td.kind = RTK_NonSerialized;   //defensive tail: every arm returns
	return td;
}

} //namespace nlang
