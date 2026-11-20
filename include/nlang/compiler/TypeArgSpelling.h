/*---
    TypeArgSpelling.h — 类型实参的共享词汇：TypeArgDesc结构、规范拼写、
    AST侧实参与VM行键（LinkKeyOf）的分类。v0.13.1运行时泛型类型信息的
    公共数据面——解析器（typeName折叠判定）、后端（描述符表与拼接记录编码）
    与加载侧拼写解析器共用一份分类，任何一侧自造拼写都会与他侧漂移
    （转型比较、intern键、typeName显示随之失配）。
---*/
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nlang
{

class SnClassDecl;
class SnInterfaceDecl;
class SnField;
class ModuleRegistry;
class SyntaxNode;

//True when the node's parent is a type declaration (class/struct/
//interface/enum) — the shared "type member" predicate of the key
//spelling and the entry-candidate scan.
bool IsTypeMember(const SyntaxNode& node);

//One type argument in both forms consumers need: the canonical name
//(display + intern key component) and the resolved structure (per-slot
//kind dispatch in the call pipeline). The spelling contract: a LEAF
//argument's spelling is its complete form ("int", "int[]"); an argument
//carrying `nested` (a nested instantiation) spells as its template's
//descriptor spelling (DescriptorSpellingOf — module-path qualified for
//owned members, bare for prelude/builtin), the tree in
//`nested` supplies the rest, and the canonical
//form is always derived through CanonicalArgSpelling (reversibility
//per Rust RFC 2603: the structure defines the spelling, so feeding a
//descriptor's stored fields back in reproduces the same key).
//`rowRef` carries the class/interface row index for class-typed
//arguments and the struct row for struct-typed ones, -1 otherwise.
//`rank` is the array rank for RTK_Array arguments (the spelling carries
//the same fact for display; runtime array checks read the field).
struct TypeArgDesc
{
	std::string spelling;
	uint8_t resolvedKind = 0;
	int32_t rowRef = -1;
	uint8_t rank = 1;
	std::vector<TypeArgDesc> nested;
};

//The argument's tree-complete canonical name: leaves pass through;
//an instantiation argument renders as Name<Arg1,Arg2> built recursively
//from its nested tree. Intern keys and Spelling() go through this —
//never join the raw spelling fields of nested arguments.
std::string CanonicalArgSpelling(const TypeArgDesc& arg);

//Type-parameter arguments are runtime-composable: rowRef encodes the
//parameter POSITION the argument reads from the receiver object's
//descriptor (the composition-record detector below). Base keeps the plain -1 row
//domain disjoint from every legal row index.
constexpr int32_t kTypeParamArgBase = -2;
inline bool IsTypeParamArg(const TypeArgDesc& arg)
{
    return arg.rowRef <= kTypeParamArgBase;
}
inline int32_t TypeParamOrdinalOf(const TypeArgDesc& arg)
{
    return kTypeParamArgBase - arg.rowRef;
}

//The link identity one field keys the VM tables under — the same
//spelling Register.cpp writes class/struct/function records with.
//Exported so the descriptor spelling and the row key cannot drift
//(prelude members key bare, package members qualified, ownerless nodes
//degrade to the unqualified name). Defined in TypeArgSpelling.cpp.
std::string LinkKeyOf(const ModuleRegistry& reg, const SnField& field);

//The descriptor-identity spelling one field keys the descriptor rows
//and intern pool under — the ROW KEY SPACE and this space are two
//key spaces with one producer each and must never cross (the two-key
//contract): LinkKeyOf keeps the v0.12.3 erasure arm for generic
//templates and instantiations alike (one shared row), while this
//function spells the full qualified link identity — the instantiation
//identity rides the spelling, not the row reference (generic_runtime_
//typeinfo_design §D2). Derivation: a declared instantiation takes its
//TEMPLATE's package (the synthetic node itself is ownerless; builtin
//instantiations have no declared template and spell bare); everything
//else takes its own package. Owned members qualify with their registry
//module path ("a.Pipe", "t7_min.Box") — the same derivation the class
//table keys use for plain classes, so leaf arguments resolve exactly,
//and a qualified template spelling lands on its shared erasure-bare
//row through the runtime's unique-suffix bridge
//(FindClassBareOrUniqueSuffix); prelude members and ownerless nodes
//stay bare. Defined in TypeArgSpelling.cpp.
std::string DescriptorSpellingOf(const ModuleRegistry& reg,
	const SnField& field);

//Build the descriptor argument list of an instantiation node: each
//element of GenericTypeArgs() maps to one TypeArgDesc — scalars by
//canonical keyword, string as "string", class/interface/struct/enum by
//their descriptor spelling (module-path qualified outside the prelude,
//bare for prelude and ownerless nodes), arrays as element spelling +
//rank marks with the
//element descriptor nested, nested instantiations recursively, and a
//type-parameter placeholder as the position encoding above (its
//spelling is the parameter name, diagnostic-only).
std::vector<TypeArgDesc> BuildTypeArgsOf(const ModuleRegistry& reg,
	const SnClassDecl& inst);

//The interface instantiation twin: same argument walk over the
//interface's GenericTypeArgs() (0.13.1c Task 8 — a generic interface's
//frame descriptor rows key the same canonical vocabulary).
std::vector<TypeArgDesc> BuildTypeArgsOf(const ModuleRegistry& reg,
	const SnInterfaceDecl& inst);

//One resolved type's descriptor argument — the shared per-type spelling
//and dispatch structure. Exported for the resolver's typeName fold
//decisions (scalar/string/link-key/array/instantiation/placeholder
//classification in one place).
TypeArgDesc BuildTypeArgOf(const ModuleRegistry& reg, const SnField& type);

//True when any argument is a type parameter — the instantiation's
//descriptor cannot be spelled at compile time and the emission must
//build it at runtime from the receiver (the composition-record path).
bool HasTypeParamArg(const std::vector<TypeArgDesc>& args);

} //namespace nlang
