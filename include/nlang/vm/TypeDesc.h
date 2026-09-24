/*-----------------------------------------------------------------------------
	nvm/intf/TypeDesc.h
	Recursive runtime-type descriptors (.nmod v1.12) — the true formal /
	return / field types a consumer needs to rebuild imported declarations.
-----------------------------------------------------------------------------*/

#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nlang
{

class SnField;
struct CompiledModule;

//Descriptor-only runtime type kinds. They never appear in the legacy
//fieldTypeKinds / returnTypeKind bytes, only inside TypeDesc — defined
//here because CompiledModule.h includes this header (the shared RTK_
//family lives next to those byte fields there). 0xFB stays clear of the
//0xFC/0xFD/0xFE default-value sentinels.
static constexpr uint8_t RTK_List          = 8;
static constexpr uint8_t RTK_Dict          = 9;
static constexpr uint8_t RTK_NonSerialized = 0xFB;

//Formal-parameter descriptor flag: the callee formal was declared `out T`
//(NF_Out on the AST side). Stub reconstruction re-stamps NF_Out so the
//consumer's call-site out-marker checks bind.
static constexpr uint8_t PTDF_Out = 0x01;

//Defensive bounds shared by the writer and the reader. Generic nesting
//(List<List<...>>) is open-ended in the language, so BOTH sides enforce
//the depth cap: the reader rejects deeper modules, and BuildTypeDesc
//degrades a container that would nest past the cap to NonSerialized —
//what ncc writes always loads. Every nesting level adds at least one
//byte, so the depth cap alone bounds a well-formed prefix's size.
static constexpr size_t kMaxTypeDescDepth = 8;
static constexpr size_t kMaxTypeDescBytes = 4096;

//Per-function formal descriptor count bound (reader-side input check;
//the writer's count comes from the AST, which the frame layout already
//caps far below this).
static constexpr size_t kMaxParamDescCount = 256;

//One recursive runtime-type descriptor. Wire form (AppendTypeDescBytes):
//  u8 kind
//  RTK_Struct / RTK_Class : u16 typeIdx (index into the module's
//                           structs / classes table)
//  RTK_Array              : 1 nested element descriptor
//  RTK_List               : 1 nested T descriptor
//  RTK_Dict               : 2 nested K, V descriptors
//  RTK_Int32 / RTK_Float / RTK_String / RTK_Boxed / RTK_Func /
//  RTK_NonSerialized     : kind byte only
//RTK_NonSerialized is the "type present but not expressible" sentinel
//(interface types, Func<...> signatures, defensive lookup misses) —
//consumers degrade to the int32 placeholder for those, matching the
//pre-v1.12 stub behavior.
struct TypeDesc
{
	uint8_t kind = RTK_NonSerialized;
	uint16_t typeIdx = 0xFFFF;
	std::vector<TypeDesc> elems;
};

//Per-formal descriptor: the parameter's type plus its serialized flags.
struct ParamTypeDesc
{
	TypeDesc type;
	uint8_t flags = 0;  //PTDF_*
};

//Append the wire form of td to out. Throws std::runtime_error on an
//arity violation (a container kind without its nested descriptors) —
//that is an upstream construction bug, not a serialization choice.
void AppendTypeDescBytes(std::vector<uint8_t>& out, const TypeDesc& td);

//Parse one descriptor from a byte buffer; the whole buffer must be
//consumed. Throws std::runtime_error on truncation, an unknown kind,
//over-deep nesting, or trailing bytes.
TypeDesc ParseTypeDescBytes(const uint8_t* pData, size_t size);

//Assert that every RTK_Struct / RTK_Class index lands inside the module's
//tables. Function records are parsed before the tables are, so this runs
//as a post-load pass. Throws std::runtime_error on violation.
void ValidateTypeDescIndices(const TypeDesc& td, size_t structCount,
	size_t classCount);

//Rewrite struct/class table indices from producer numbering to consumer
//numbering (import merge). The merge maps are total over each kind's
//producer indices (every producer index gets a dedup-or-push entry), so
//a miss is an upstream construction bug — .at() makes it loud instead
//of silently keeping a stale index that can alias a wrong type.
void RemapTypeDesc(TypeDesc& td,
	const std::unordered_map<uint32_t, uint32_t>& structMap,
	const std::unordered_map<uint32_t, uint32_t>& classMap);

//Build the descriptor for one resolved type (a declaration field or an
//interned array token; never a raw syntactic expression). Names it
//cannot resolve through mod's tables degrade to RTK_NonSerialized, and
//so does a container nested at kMaxTypeDescDepth (see the caps note).
//Defined in TypeDesc.cpp — vm-internal, may include compiler headers.
TypeDesc BuildTypeDesc(const SnField* pType, const CompiledModule& mod,
	size_t depth = 0);

} //namespace nlang
