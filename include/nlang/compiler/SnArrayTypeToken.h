/*-----------------------------------------------------------------------------
	ncomp/intf/SnArrayTypeToken.h
	The interned first-class array type token ("array of T", 0.7.3 B).
-----------------------------------------------------------------------------*/

#pragma once
#include "SyntaxNode.h"

namespace nlang
{

//The interned "array of T" type token. A leaf SnField acting as a
//first-class type: one instance per element type per translation unit
//(BuildEnvironment interning), so pointer equality IS type equality
//(hash-consing). Construction is closed — only
//BuildEnvironment::InternArrayTypeToken mints tokens; an un-interned
//instance would silently break the pointer-identity contract.
class NLANG_COMPILER_API SnArrayTypeToken : public SnField
{
	friend class BuildEnvironment;
	typedef SnField Super_;
public:
	static const NodeKind	s_Kind			= NK_ArrayTypeToken;
	//Mirror the type-field sibling family (SnStructDecl etc.: NF_Type |
	//NF_Field | NF_Plain) plus the interning extras. IsTypeField() checks
	//NF_Type|NF_Field — omitting NF_Field misclassifies the token as a
	//non-type node. NF_DontDelete keeps the tree-side deleters off the
	//token (the interning table owns it); NF_Hidden keeps it out of the
	//user-visible surface.
	static const NodeBits	s_DefaultFlags
		= NF_Type | NF_Field | NF_Plain | NF_DontDelete | NF_Hidden;
public:
	//The element type field (primitive field, SnClassDecl, struct, enum,
	//or another token for T[][]). Never null. Non-owning: the element
	//stays owned by its declaring tree/environment.
	SnField *ElemTypeOf() const
	{
		return m_pElemType;
	}

	//The token IS the array-type witness — no side table needed.
	bool IsArrayType() const override
	{
		return true;
	}

	//A type token's own type is itself.
	SnField *EvalDataType() const override
	{
		return const_cast<SnArrayTypeToken*>(this);
	}

	SnField *FindField(const std::string& sName) const override;

	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor&) override;

	//Display form, e.g. "int[]" / "int[][]".
	std::string ToString() const override;
private:
	//Private: single-entry construction through the interning table.
	SnArrayTypeToken(SnField *pElemType, const ISourceLocation &loc);

	ImmutableNodeList *ChildrenPtr() const override;

	SnField *m_pElemType;
};

} //namespace nlang
