#include "CastInfo.h"
#include "SnMisc.h"
#include <nlang/runtime/PrimitiveTypes.h>

namespace nlang
{

TypeCastKind TypeCastInfo::s_CastTable[NK_DT_COUNT][NK_DT_COUNT];

//0.7.5: one derivation rule per scalar pair replaces the hand-written
//matrix — the source of truth is the registry's category+rank.
static TypeCastKind DeriveScalarCast(const ScalarPrimInfo &src,
                                     const ScalarPrimInfo &dst)
{
	if (src.kind == dst.kind)
		return TCK_Same;
	//bool is isolated: no implicit or explicit scalar conversion in
	//either direction (comparisons produce bool; nothing consumes it
	//numerically). `b as bool`-style identities are Same above.
	if (src.category == PC_Bool || dst.category == PC_Bool)
		return TCK_None;
	//char: implicit target is string only (the anchor row in StaticInit);
	//char ↔ numeric is explicit both ways (`c as int` = code point,
	//`n as char` validates the scalar value at run time).
	if (src.category == PC_Char || dst.category == PC_Char)
		return TCK_Explicit;
	//integer → float of any width: implicit (Java/C# convention, the
	//lossy pairs warn — the value-range warnings land with the integer
	//family); float → integer: explicit.
	if (PrimCategoryIsNumeric(src.category)
		&& PrimCategoryIsNumeric(dst.category))
	{
		bool srcInt = src.category != PC_Float;
		bool dstInt = dst.category != PC_Float;
		if (srcInt && !dstInt) return TCK_Auto;              // int → f32/f64
		if (!srcInt && dstInt) return TCK_Explicit;          // f32/f64 → int
		if (PrimDomainContained(src.category, src.rank,
			dst.category, dst.rank))
			return TCK_Auto;                                  // widening
		return TCK_Explicit;                                  // narrowing / sign-change
	}
	return TCK_None;
}

//The →string column and the string row: every scalar coerces to string
//(Auto — the runtime rendering family); string converts to nothing
//(None) — spec §2.2 lists string→number as an explicit `as` conversion,
//but 0.7.5 ships it as the toInt/toLong/toDouble/toFloat/toBool METHODS
//only, so `s as int` stays a compile error this release (marked spec
//deviation, the known-limitations page records it).
static void InitStringColumn(
	TypeCastKind (&table)[NK_DT_COUNT][NK_DT_COUNT])
{
	table[NK_Int32][NK_String]  = TCK_Auto;
	table[NK_Float][NK_String]  = TCK_Auto;
	table[NK_Byte][NK_String]   = TCK_Auto;
	table[NK_UByte][NK_String]  = TCK_Auto;
	table[NK_Short][NK_String]  = TCK_Auto;
	table[NK_UShort][NK_String] = TCK_Auto;
	table[NK_UInt32][NK_String] = TCK_Auto;
	table[NK_Long][NK_String]   = TCK_Auto;
	table[NK_ULong][NK_String]  = TCK_Auto;
	table[NK_Double][NK_String] = TCK_Auto;
	//bool → string renders "true"/"false".
	table[NK_Bool][NK_String]   = TCK_Auto;
	//char → string: Auto (encodes one UTF-8 code point, 1–4 bytes).
	table[NK_Char][NK_String]   = TCK_Auto;
	table[NK_String][NK_Int32]  = TCK_None;
	table[NK_String][NK_Float]  = TCK_None;
	table[NK_String][NK_String] = TCK_Same;
}

void TypeCastInfo::StaticInit()
{
	//Rule-derived primitive matrix (spec §2.2). Row/col range covers all
	//data-type kinds; non-scalar cells stay TCK_None (their verdicts are
	//computed by CalcCastKind's special paths, never this table).
	for (size_t i = 0; i < NK_DT_COUNT; ++i)
		for (size_t j = 0; j < NK_DT_COUNT; ++j)
			s_CastTable[i][j] = TCK_None;

	for (size_t i = 0; i < kScalarPrimCount; ++i)
	{
		const auto &src = kScalarPrims[i];
		for (size_t j = 0; j < kScalarPrimCount; ++j)
		{
			const auto &dst = kScalarPrims[j];
			s_CastTable[src.kind][dst.kind] =
				DeriveScalarCast(src, dst);
		}
	}
	InitStringColumn(s_CastTable);
	//void row/col (Phase 13 comment preserved): all None except Same.
	s_CastTable[NK_Void][NK_Void]     = TCK_Same;
	//type row/col: Same on the diagonal only (already None elsewhere).
	s_CastTable[NK_Type][NK_Type]     = TCK_Same;
#ifndef NDEBUG
	for (size_t i = 0; i < NK_DT_COUNT; ++i)
		for (size_t j = 0; j < NK_DT_COUNT; ++j)
		{
			assert(s_CastTable[i][j] != 0 &&
				"The cast table is not initialized properly");
		}
#endif
}

void TypeCastInfo::CalcCastKind()
{
	if (!m_pSource || !m_pTarget)
	{
		m_Kind = TCK_None;
		return;
	}
	auto srcKind = m_pSource->Kind();
	auto tgtKind = m_pTarget->Kind();
	//0.7.3 B: interned array tokens never enter the enum remap or the
	//dense cast table — pointer identity rules, same as the struct
	//branch below.
	if (srcKind == NK_ArrayTypeToken || tgtKind == NK_ArrayTypeToken)
	{
		if (srcKind == NK_ArrayTypeToken && tgtKind == NK_ArrayTypeToken)
		{
			//Same interned token → Same; different element → None.
			m_Kind = (m_pSource == m_pTarget) ? TCK_Same : TCK_None;
			return;
		}
		//Array → string = Auto (all-position toString coercion,
		//mirroring the class → string branch below).
		if (srcKind == NK_ArrayTypeToken && tgtKind == NK_String)
		{
			m_Kind = TCK_Auto;
			return;
		}
		//Null bridge: int source (the null literal's type) × array
		//target = Auto no-op; FixupExprType's null-only gate rejects
		//the non-null int. Mirrors the int → ClassDecl bridge below.
		if (srcKind == NK_Int32 && tgtKind == NK_ArrayTypeToken)
		{
			m_Kind = TCK_Auto;
			return;
		}
		m_Kind = TCK_None;
		return;
	}
	//Enum types are int32 at runtime — treat them as NK_Int32 for casting.
	if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
	if (tgtKind == NK_EnumDecl) tgtKind = NK_Int32;
	//Struct types: same struct type → TCK_Same; otherwise incompatible.
	if (srcKind == NK_StructDecl && tgtKind == NK_StructDecl)
	{
		m_Kind = (m_pSource == m_pTarget) ? TCK_Same : TCK_None;
		return;
	}
	if (srcKind == NK_StructDecl || tgtKind == NK_StructDecl)
	{
		m_Kind = TCK_None;
		return;
	}
	//Class types: same class → TCK_Same; subclass to parent → TCK_Same (implicit upcast);
	//parent to subclass → TCK_Downcast (explicit, via `as`); otherwise incompatible.
	if (srcKind == NK_ClassDecl && tgtKind == NK_ClassDecl)
	{
		if (m_pSource == m_pTarget)
		{
			m_Kind = TCK_Same;
			return;
		}
		//Phase 13: function handles do not participate in class upcasting.
		//The synthetic func<...> declaration is an SnClassDecl, so without
		//this guard `Object o = f` would take the Object special case below
		//as a TCK_Same no-op and box the handle into an untracked slot.
		if (static_cast<const SnClassDecl*>(m_pSource)->IsFuncType()
			|| static_cast<const SnClassDecl*>(m_pTarget)->IsFuncType())
		{
			m_Kind = TCK_None;
			return;
		}
		//Phase 8e-1: implicit upcast to Object. Object is the universal root
		//at the VM level (VmBackend injects superClassIdx=Object's idx for
		//every user class with no explicit parent), but the AST-level
		//SnClassDecl::SuperClass() chain doesn't include Object. Special-case
		//it here: any class → Object = TCK_Same (no-op, same heap idx).
		if (m_pTarget && m_pTarget->Name() == "Object")
		{
			m_Kind = TCK_Same;
			return;
		}
		//Upcast check: walk SOURCE's chain, find target → TCK_Same (implicit, no-op).
		auto *pSrc = static_cast<const SnClassDecl*>(m_pSource);
		auto *pParent = pSrc->SuperClass();
		while (pParent)
		{
			if (pParent == m_pTarget)
			{
				m_Kind = TCK_Same;
				return;
			}
			pParent = pParent->SuperClass();
		}
		//Phase 8e-1.5: Downcast check: walk TARGET's chain, find source → TCK_Downcast.
		//Means: target IS-A source (source is an ancestor of target). Honored only via
		//`expr as SubClass` (SnAsExpr); FixupExprType rejects TCK_Downcast for implicit flows.
		auto *pTgt = static_cast<const SnClassDecl*>(m_pTarget);
		auto *pCur = pTgt->SuperClass();
		while (pCur)
		{
			if (pCur == m_pSource)
			{
				m_Kind = TCK_Downcast;
				return;
			}
			pCur = pCur->SuperClass();
		}
		//Phase 8e-1.5: implicit downcast from Object. Symmetric to the
		//upcast special-case above: Object → any user class is a downcast
		//(runtime-checked via OP_CheckCast in `o as Foo`).
		if (m_pSource && m_pSource->Name() == "Object")
		{
			m_Kind = TCK_Downcast;
			return;
		}
		m_Kind = TCK_None;
		return;
	}
	//Class to interface: implicit upcast when the class (or any ancestor)
	//declares "implements <target>". Same pointer at runtime — no cast.
	if (srcKind == NK_ClassDecl && tgtKind == NK_InterfaceDecl)
	{
		auto *pSrc = static_cast<const SnClassDecl*>(m_pSource);
		auto *pCur = pSrc;
		while (pCur)
		{
			for (auto *pIface : pCur->ImplementsList())
			{
				if (pIface == m_pTarget)
				{
					m_Kind = TCK_Same;
					return;
				}
			}
			pCur = pCur->SuperClass();
		}
		m_Kind = TCK_None;
		return;
	}
	//Allow int (null literal, KT_Null is int32) to be assigned to interface type.
	if (srcKind == NK_Int32 && tgtKind == NK_InterfaceDecl)
	{
		m_Kind = TCK_Auto;
		return;
	}
	//Interface to interface: identity only.
	if (srcKind == NK_InterfaceDecl && tgtKind == NK_InterfaceDecl)
	{
		m_Kind = (m_pSource == m_pTarget) ? TCK_Same : TCK_None;
		return;
	}
	if (srcKind == NK_ClassDecl || tgtKind == NK_ClassDecl)
	{
		//Phase 8e-1: primitive (registry scalar or string) → Object =
		//implicit box. 0.7.5: the hardcoded int/float/string triple is
		//registry-driven now — every scalar primitive (bool today, the
		//full integer family in P4) boxes without touching this file.
		//Must be checked BEFORE the generic null-literal rule below so that
		//`Object o = 5` produces TCK_Box (and emits OP_Box), not TCK_Auto
		//(which would skip boxing entirely and store the raw int).
		//Object is recognized by name ("Object") since it has no AST parent.
		if ((ScalarPrimIndexOf(srcKind) >= 0 || srcKind == NK_String)
			&& tgtKind == NK_ClassDecl
			&& m_pTarget && m_pTarget->Name() == "Object")
		{
			m_Kind = TCK_Box;
			return;
		}
		//Allow int (null literal) to be assigned to class type.
		//Fires for `Foo f = null` (non-Object class targets) where source
		//is the int-typed null literal. TCK_Auto is a no-op at runtime
		//(null stays as heap idx 0).
		if (srcKind == NK_Int32 && tgtKind == NK_ClassDecl)
		{
			m_Kind = TCK_Auto;
			return;
		}
		//Phase 8e-1.5: Object → primitive = explicit unbox (TCK_Unbox).
		//Only honored through `o as int` (SnAsExpr). Assignments from Object
		//to primitive still reject (TCK_None) to keep implicit flows safe.
		//0.7.5: registry-driven target set (see the box branch above).
		if (srcKind == NK_ClassDecl && m_pSource && m_pSource->Name() == "Object"
			&& (ScalarPrimIndexOf(tgtKind) >= 0 || tgtKind == NK_String))
		{
			m_Kind = TCK_Unbox;
			return;
		}
		//Phase 8e-9b: any class → string = TCK_Auto. VmBackend SnCastExpr emits
		//a virtual toString() call. Mirrors 8e-9a primitive→string coercion.
		if (srcKind == NK_ClassDecl && tgtKind == NK_String)
		{
			m_Kind = TCK_Auto;
			return;
		}
		m_Kind = TCK_None;
		return;
	}
	if (srcKind >= NK_DT_COUNT || tgtKind >= NK_DT_COUNT)
	{
		m_Kind = TCK_None;
		return;
	}
	const auto k = s_CastTable[srcKind][tgtKind];
	m_Kind = k;
}


} //namespace nlang
