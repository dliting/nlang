#include "CastInfo.h"
#include "SnMisc.h"

namespace nlang
{

TypeCastKind TypeCastInfo::s_CastTable[NK_DT_COUNT][NK_DT_COUNT];

void TypeCastInfo::StaticInit()
{
	s_CastTable[NK_Int32][NK_Int32]		= TCK_Same;
	s_CastTable[NK_Int32][NK_Float]		= TCK_Auto;
	s_CastTable[NK_Int32][NK_String]	= TCK_Auto;
	s_CastTable[NK_Int32][NK_Type]		= TCK_None;

	s_CastTable[NK_Float][NK_Int32]		= TCK_Auto;
	s_CastTable[NK_Float][NK_Float]		= TCK_Same;
	s_CastTable[NK_Float][NK_String]	= TCK_Auto;
	s_CastTable[NK_Float][NK_Type]		= TCK_None;

	s_CastTable[NK_String][NK_Int32]	= TCK_None;
	s_CastTable[NK_String][NK_Float]	= TCK_None;
	s_CastTable[NK_String][NK_String]	= TCK_Same;
	s_CastTable[NK_String][NK_Type]		= TCK_None;

	s_CastTable[NK_Type][NK_Int32]		= TCK_None;
	s_CastTable[NK_Type][NK_Float]		= TCK_None;
	s_CastTable[NK_Type][NK_String]		= TCK_None;
	s_CastTable[NK_Type][NK_Type]		= TCK_Same;

	//Phase 13: void exists only as a Func<...> return slot and never
	//reaches expression casting as a value type, but NK_DT_COUNT grew
	//with the new kind — fill the row/column so the init assert holds
	//and any accidental use yields TCK_None instead of an unset entry.
	s_CastTable[NK_Void][NK_Int32]		= TCK_None;
	s_CastTable[NK_Void][NK_Float]		= TCK_None;
	s_CastTable[NK_Void][NK_String]		= TCK_None;
	s_CastTable[NK_Void][NK_Type]		= TCK_None;
	s_CastTable[NK_Void][NK_Void]		= TCK_Same;
	s_CastTable[NK_Int32][NK_Void]		= TCK_None;
	s_CastTable[NK_Float][NK_Void]		= TCK_None;
	s_CastTable[NK_String][NK_Void]		= TCK_None;
	s_CastTable[NK_Type][NK_Void]		= TCK_None;
#ifndef NDEBUG
	for (size_t i = NK_Int32; i < NK_DT_COUNT; ++i)
		for (size_t j = NK_Int32; j < NK_DT_COUNT; ++j)
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
		//The synthetic Func<...> declaration is an SnClassDecl, so without
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
		//Phase 8e-1: primitive (int/float/string) → Object = implicit box.
		//Must be checked BEFORE the generic null-literal rule below so that
		//`Object o = 5` produces TCK_Box (and emits OP_Box), not TCK_Auto
		//(which would skip boxing entirely and store the raw int).
		//Object is recognized by name ("Object") since it has no AST parent.
		if ((srcKind == NK_Int32 || srcKind == NK_Float || srcKind == NK_String)
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
		if (srcKind == NK_ClassDecl && m_pSource && m_pSource->Name() == "Object"
			&& (tgtKind == NK_Int32 || tgtKind == NK_Float || tgtKind == NK_String))
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
