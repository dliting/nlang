/*-----------------------------------------------------------------------------
ncomp/intf/NodeConsts.h
This file define some common constants, macros and helper data structures for
manipulating nodes in nlang.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"

namespace nlang
{

//Node bits combination from high to low.
//32 bits = flags bits + access type bits + kind bits; 
typedef uint32 NodeBits;

#define NODE_KIND_BITS		6
#define FIELD_ACCESS_BITS	2
#define NODE_FLAG_BITS		24

const size_t	NODE_KIND_LIMIT = 1 << NODE_KIND_BITS;

const size_t	NODE_FLAG_LIMIT = 1 << NODE_FLAG_BITS;

//Primitive data type nodes.
#define PRIMITIVE_TYPE_NODE_DECL(MACRO_IMPL)								\
	MACRO_IMPL(Int32)		/* 32-bit signed int */							\
	MACRO_IMPL(Float)		/* 32-bit IEEE float */							\
	MACRO_IMPL(String)		/* UTF-8 string */								

//Extend data type nodes.
#define EXTEND_TYPE_NODE_DECL(MACRO_IMPL)									\
	MACRO_IMPL(Type)		/* type of types */

//Builtin data type nodes.
#define BUILTIN_TYPE_NODE_DECL(MACRO_IMPL)									\
	PRIMITIVE_TYPE_NODE_DECL(MACRO_IMPL)									\
	MACRO_IMPL(Type)

//All data types nodes.
#define TYPE_NODE_DECL(MACRO_IMPL)											\
	PRIMITIVE_TYPE_NODE_DECL(MACRO_IMPL)									\
	EXTEND_TYPE_NODE_DECL(MACRO_IMPL)									

//Runtime data nodes.
#define RUNTIME_DATA_NODE_DECL(MACRO_IMPL)									\
	MACRO_IMPL(Function)		/* a function */							\
	MACRO_IMPL(FormalParam)		/* a formal parameter of a function */

//Runtime miscellaneous nodes.
#define RUNTIME_MISC_NODE_DECL(MACRO_IMPL)									\
	MACRO_IMPL(Namespace)		/* a namespace */							\

//Runtime node type declaration macro.
#define RUNTIME_NODE_DECL(MACRO_IMPL)										\
	TYPE_NODE_DECL(MACRO_IMPL)												\
	RUNTIME_DATA_NODE_DECL(MACRO_IMPL)										\
	RUNTIME_MISC_NODE_DECL(MACRO_IMPL)

//#define RUNTIME_NODE_TYPES_DECL(MACRO_IMPL)								\
//	MACRO_IMPL(Bool)		/* boolean type */								\
//	MACRO_IMPL(Byte)		/* signed byte. */								\
//	MACRO_IMPL(UByte)		/* unsigned byte. */							\
//	MACRO_IMPL(Int)			/* 32-bit signed int */							\
//	MACRO_IMPL(UInt)		/* 32-bit unsigned int */						\
//	MACRO_IMPL(Float)		/* 32-bit IEEE float. */						\
//	MACRO_IMPL(Char)		/* UTF-8 character */							\
//	MACRO_IMPL(String)		/* UTF-8 string */								\
//	MACRO_IMPL(Struct)		/* a simple user defined data structure */		\
//	MACRO_IMPL(Functor)		/* function delegate */							\
//	MACRO_IMPL(Interface)	/* abstract methods bundle */					\
//	MACRO_IMPL(Class)		/* type of an object */							\
//	MACRO_IMPL(Array)		/* fixed size array */							\
//	MACRO_IMPL(Vect)		/* a indexed list */							\
//	MACRO_IMPL(Dict)		/* a map ordered by keys */						\
//	MACRO_IMPL(Enum)		/* enumeration type */							\
//	MACRO_IMPL(Function)	/* a function */								\
//	MACRO_IMPL(Namespace)	/* a namespace */								\
//	MACRO_IMPL(Property)	/* a property item of a compound type */		\
//	MACRO_IMPL(FormalParam)	/* a formal parameter of a function */

typedef uint8 NodeKind;

//Runtime node type enumerations.
enum RuntimeNodeKind : NodeKind
{
#define MACRO_IMPL(T) NK_##T,
	PRIMITIVE_TYPE_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
	NK_PRIMITIVE_DT_COUNT,

	NK_EXTEND_DT_BEFORE = NK_PRIMITIVE_DT_COUNT - 1,
#define MACRO_IMPL(T) NK_##T,
	EXTEND_TYPE_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
	NK_DT_COUNT,	//# of data type node kinds.

	NK_DD_BEFORE = NK_DT_COUNT - 1,
#define MACRO_IMPL(T) NK_##T,
	RUNTIME_DATA_NODE_DECL(MACRO_IMPL)
	RUNTIME_MISC_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL

	NK_RT_END		//# of runtime node kinds.
};

//Can the specified kind of node be the parent of a function?
//Extended check for compile-time node kinds is in SyntaxNodeConsts.h.
inline bool CanBeFuncParent(NodeKind k)
{
	return k == NK_Namespace;
}

inline bool IsPrimitiveType(NodeKind k)
{
	return k < NK_PRIMITIVE_DT_COUNT;
}

inline bool IsBuiltinType(NodeKind k)
{
	return IsPrimitiveType(k) || k == NK_Type; //TODO
}

//Access type of a field.
#define FIELD_ACCESS_TYPE_DECL(MACRO_IMPL)									\
	MACRO_IMPL(Default)														\
	MACRO_IMPL(Private)														\
	MACRO_IMPL(Protected)													\
	MACRO_IMPL(Public)														

enum FieldAccessType : uint8
{
#define MACRO_IMPL(name) FA_##name,
	FIELD_ACCESS_TYPE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
};

//Validate the the defining order of node modifiers.
static_assert(FA_Public > FA_Protected && FA_Protected > FA_Private,
	"Member access type definition order error.");

//Runtime node flags.
#define RUNTIEM_NODE_FLAGS_DECL(MACRO_IMPL)									\
	MACRO_IMPL(Type, 		 0,	"type declaration")							\
	MACRO_IMPL(Data, 		 1,	"data declaration")							\
	MACRO_IMPL(Field,		 2, "field")									\
	MACRO_IMPL(Plain,		 3, "plain type")								\
	MACRO_IMPL(Const, 		 4,	"constant")									\
	MACRO_IMPL(DontDelete, 	 5,	"should not be deleted")					\
	MACRO_IMPL(Hidden, 		 6,	"invisible to user")						\
	MACRO_IMPL(Reference, 	 7,	"value reference")							\
	MACRO_IMPL(Optional, 	 8,	"optional param")							\
	MACRO_IMPL(Static,		 9,	"static storage")							\
	MACRO_IMPL(Abstract,	10,	"abstract type")							\
	MACRO_IMPL(Native,		11,	"native function")							\
	MACRO_IMPL(Virtual,		12,	"virtual function")							\
	MACRO_IMPL(External,	13,	"external function")						

enum RuntimeNodeFlag : NodeBits
{
#define MACRO_IMPL(name, offset, desc) NF_##name = 1 << offset,
	RUNTIEM_NODE_FLAGS_DECL(MACRO_IMPL)
	NF_RT_END
#undef MACRO_IMPL
};

//All the flags can be used for a syntax node.
#define SYNTAX_NODE_FLAG_DECL(MACRO_IMPL) \
	RUNTIEM_NODE_FLAGS_DECL(MACRO_IMPL) \
	COMPILE_ONLY_NODE_FLAG_DECL(MACRO_IMPL) 

const NodeBits NF_NONE			= 0;

const NodeBits RUNTIME_NODE_FLAGS_MASK = ~(NF_RT_END - 2);

static_assert(NF_RT_END < NODE_FLAG_LIMIT, "Node flag definition error.");

//RnFunction only node flags.
const NodeBits NF_FuncOnlyFlags = NF_Abstract | NF_Native | NF_Virtual;

#define GLOBAL_NAMESPACE_NAME "$GlobalNamespace"

} //namespace nlang
