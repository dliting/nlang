/*---
TypeDesc.cpp — .ncu v1.12 type descriptors (serialize / parse).

The AST-dependent builder BuildTypeDesc lives in TypeDescBuild.cpp
(the layering split keeps this TU wire-only).

One grammar shared by the writer (VmBackend captures, ModuleSaver emits)
and the reader (ModuleLoader parses + validates). Keep AppendTypeDescBytes
and ParseOne in lockstep with the struct comment in TypeDesc.h. The
consumer-side index bounds check and the import-merge index rewrite are a
different responsibility and live in TypeDescIndices.cpp.
---*/
#include "nlang/core/TypeDesc.h"
//RTK_* kind constants (also the TypeDesc field-width contract).
#include "nlang/core/CompiledModule.h"
#include <stdexcept>
#include <string>

namespace nlang {

//RTK_Func body: u8 paramCount, then the params and the return as
//nested descriptors, each followed by its u8 qualifier flags
//(mirror of ParseFuncElems).
void AppendTypeDescBytes(std::vector<uint8_t>& out, const TypeDesc& td);

void AppendFuncElems(std::vector<uint8_t>& out, const TypeDesc& td)
{
	if (td.elems.empty())
		throw std::runtime_error(
			"type descriptor arity violation: func without its "
			"return descriptor");
	out.push_back(static_cast<uint8_t>(td.elems.size() - 1));
	for (size_t i = 0; i < td.elems.size(); ++i)
	{
		AppendTypeDescBytes(out, td.elems[i]);
		//v2.21: the qualifier flags ride after each element — a
		//descriptor-only construction without the parallel vector
		//serializes unmarked positions (all-zero bytes).
		out.push_back(i < td.elemFlags.size() ? td.elemFlags[i] : 0);
	}
}

void AppendTypeDescBytes(std::vector<uint8_t>& out, const TypeDesc& td)
{
	out.push_back(td.kind);
	if (td.kind == RTK_TypeParam)
	{
		//v0.13.1 D7: the declared parameter position rides the leaf —
		//the consumer stub names it from the owning row's typeParamNames.
		out.push_back(td.typeParamOrdinal);
	}
	//v0.13.9 Task 12 (D14): RTK_Interface carries the same u16 row
	//index as struct/class (into the interfaces table — identity tier).
	else if (td.kind == RTK_Struct || td.kind == RTK_Class
		|| td.kind == RTK_Interface)
	{
		out.push_back(static_cast<uint8_t>(td.typeIdx & 0xFF));
		out.push_back(static_cast<uint8_t>(td.typeIdx >> 8));
	}
	else if (td.kind == RTK_Array)
	{
		if (td.elems.empty())
			throw std::runtime_error(
				"type descriptor arity violation: container kind without "
				"its nested descriptor");
		AppendTypeDescBytes(out, td.elems[0]);
		//v2.11: the rank rides after the element descriptor, so the
		//consumer's stub rebuild creates T[,] instead of degrading to T[].
		out.push_back(td.rank);
	}
	else if (td.kind == RTK_List)
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
	else if (td.kind == RTK_Func)
		AppendFuncElems(out, td);
}

namespace {

TypeDesc ParseOne(const uint8_t* pData, size_t size, size_t& pos,
	size_t depth);

//RTK_Func body: u8 paramCount, then the params and the return as
//nested descriptors, each followed by its u8 qualifier flags (mirror
//of AppendFuncElems — v2.21 layout).
void ParseFuncElems(TypeDesc& td, const uint8_t* pData, size_t size,
	size_t& pos, size_t depth)
{
	if (pos >= size)
		throw std::runtime_error(
			"Invalid module: truncated type descriptor");
	const uint8_t paramCount = pData[pos++];
	for (uint8_t elem = 0; elem < paramCount + 1; ++elem)
	{
		td.elems.push_back(ParseOne(pData, size, pos, depth + 1));
		if (pos >= size)
			throw std::runtime_error(
				"Invalid module: truncated type descriptor");
		td.elemFlags.push_back(pData[pos++]);
	}
}

//RTK_Array body: the element descriptor, then the v2.11 rank byte
//(the cap mirrors the language-side kMaxArrayRank). v0.12.7: rank 0 is
//the unannotated spelling (Array<E> — the bracket annotation lives on
//the value, not the type), 1..kMaxArrayRank are bracket annotations.
void ParseArrayBody(TypeDesc& td, const uint8_t* pData, size_t size,
	size_t& pos, size_t depth)
{
	td.elems.push_back(ParseOne(pData, size, pos, depth + 1));
	if (pos >= size)
		throw std::runtime_error(
			"Invalid module: truncated type descriptor");
	td.rank = pData[pos++];
	if (td.rank > kMaxArrayRank)
		throw std::runtime_error(
			"Invalid module: array descriptor rank out of range");
}

//RTK_Struct/RTK_Class body: the little-endian u16 type-table index.
static uint16_t ParseTypeIndex(const uint8_t* pData, size_t size,
	size_t& pos)
{
	if (size - pos < 2)
		throw std::runtime_error(
			"Invalid module: truncated type descriptor");
	const uint16_t idx = static_cast<uint16_t>(pData[pos])
		| (static_cast<uint16_t>(pData[pos + 1]) << 8);
	pos += 2;
	return idx;
}

//RTK_TypeParam body: the declared parameter position (u8) — the
//consumer stub names it from the owning row's typeParamNames.
static uint8_t ParseTypeParamOrdinal(const uint8_t* pData, size_t size,
	size_t& pos)
{
	if (pos >= size)
		throw std::runtime_error(
			"Invalid module: truncated type descriptor");
	return pData[pos++];
}

//String/boxed/typeparam leaf body: kind-only for string/boxed, and the
//ordinal byte for a type parameter (the kind guard keeps the read off
//the kind-only leaves).
static void ParseTypeParamLeaf(TypeDesc& td, const uint8_t* pData,
	size_t size, size_t& pos)
{
	if (td.kind == RTK_TypeParam)
		td.typeParamOrdinal =
			ParseTypeParamOrdinal(pData, size, pos);
}

//RTK_Dict body: the key and value descriptors, in wire order.
void ParseDictBody(TypeDesc& td, const uint8_t* pData, size_t size,
	size_t& pos, size_t depth)
{
	td.elems.push_back(ParseOne(pData, size, pos, depth + 1));
	td.elems.push_back(ParseOne(pData, size, pos, depth + 1));
}

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
		//Row-index leaves: struct/class/interface table rows (the
		//interface row is the v0.13.9 Task 12 D14 identity tier).
		case RTK_Struct:
		case RTK_Class:
		case RTK_Interface:
			td.typeIdx = ParseTypeIndex(pData, size, pos);
			break;
		case RTK_Array:
			ParseArrayBody(td, pData, size, pos, depth);
			break;
		case RTK_List:
			td.elems.push_back(ParseOne(pData, size, pos, depth + 1));
			break;
		case RTK_Dict:
			ParseDictBody(td, pData, size, pos, depth);
			break;
		case RTK_Func:
			ParseFuncElems(td, pData, size, pos, depth);
			break;
		case RTK_String:
		case RTK_Boxed:
		case RTK_TypeParam:
			ParseTypeParamLeaf(td, pData, size, pos);
			break;
		//Void leaf: legal only as an RTK_Func return slot — a top-level
		//one degrades to the int32 placeholder on the consumer side.
		case RTK_Void:
		case RTK_NonSerialized:
			break;
		default:
			//0.7.5: every scalar kind is a kind-byte-only leaf —
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

} //namespace nlang
