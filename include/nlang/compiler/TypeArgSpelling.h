/*---
    TypeArgSpelling.h — 类型实参的共享词汇：TypeArgDesc结构、规范拼写、
    AST侧实参与VM行键（LinkKeyOf）的分类。v0.13.1运行时泛型类型信息的
    公共数据面——解析器（typeName折叠判定）、后端（描述符表与配方编码）
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
class SnField;
class ModuleRegistry;
class SyntaxNode;

//True when the node's parent is a type declaration (class/struct/
//interface/enum) — the shared "type member" predicate of the key
//spelling and the entry-candidate scan.
bool IsTypeMember(const SyntaxNode& node);

//One type argument in both forms consumers need: the canonical spelling
//(display + intern key component) and the resolved structure (per-slot
//kind dispatch in the call pipeline). The spelling contract: a LEAF
//argument's spelling is its complete form ("int", "int[]"); an argument
//carrying `nested` (a nested instantiation) spells as its template's
//BARE name — the tree in `nested` supplies the rest, and the canonical
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

//The argument's tree-complete canonical spelling: leaves pass through;
//an instantiation argument renders as Name<Arg1,Arg2> built recursively
//from its nested tree. Intern keys and Spelling() go through this —
//never join the raw spelling fields of nested arguments.
std::string CanonicalArgSpelling(const TypeArgDesc& arg);

//Type-parameter arguments are runtime-composable: rowRef encodes the
//parameter POSITION the argument reads from the receiver object's
//descriptor (the recipe detector below). Base keeps the plain -1 row
//domain disjoint from every legal row index.
constexpr int32_t kTypeParamArgBase = -2;
inline bool IsTypeParamArg(const TypeArgDesc& arg)
{ return arg.rowRef <= kTypeParamArgBase; }
inline int32_t TypeParamOrdinalOf(const TypeArgDesc& arg)
{ return kTypeParamArgBase - arg.rowRef; }

//The link identity one field keys the VM tables under — the same
//spelling Register.cpp writes class/struct/function records with.
//Exported so the descriptor spelling and the row key cannot drift
//(prelude members key bare, package members qualified, ownerless nodes
//degrade to the bare name). Defined in TypeArgSpelling.cpp.
std::string LinkKeyOf(const ModuleRegistry& reg, const SnField& field);

//Build the descriptor argument list of an instantiation node: each
//element of GenericTypeArgs() maps to one TypeArgDesc — scalars by
//canonical keyword, string as "string", class/interface/struct/enum by
//their link key (qualified for package members, bare for prelude and
//ownerless nodes), arrays as element spelling + rank marks with the
//element descriptor nested, nested instantiations recursively, and a
//type-parameter placeholder as the position encoding above (its
//spelling is the parameter name, diagnostic-only).
std::vector<TypeArgDesc> BuildTypeArgsOf(const ModuleRegistry& reg,
	const SnClassDecl& inst);

//One resolved type's descriptor argument — the shared per-type spelling
//and dispatch structure. Exported for the resolver's typeName fold
//decisions (scalar/string/link-key/array/instantiation/placeholder
//classification in one place).
TypeArgDesc BuildTypeArgOf(const ModuleRegistry& reg, const SnField& type);

//True when any argument is a type parameter — the instantiation's
//descriptor cannot be spelled at compile time and the emission must
//build it at runtime from the receiver (the recipe arm).
bool HasTypeParamArg(const std::vector<TypeArgDesc>& args);

} //namespace nlang
