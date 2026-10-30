/*--- GenericInstDescriptor.h — 泛型实例化描述符：模板裸名＋类型实参表，
    规范拼写为无空格形Base<Arg1,Arg2>，按拼写intern。字典模型的运行时
    类型实参载体——对象头（Task 6）、隐藏调用槽（Task 7a）、ABI回调
    （Task 11）与序列化（Task 12）的共享数据面。
---*/
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nlang
{

//One type argument in both forms consumers need: the canonical spelling
//(display + intern key component) and the resolved structure (per-slot
//kind dispatch in the call pipeline). The spelling contract: a LEAF
//argument's spelling is its complete form ("int", "int[]"); an argument
//carrying `nested` (a nested instantiation) spells as its template's
//BARE name — the tree in `nested` supplies the rest, and the canonical
//form is always derived through CanonicalArgSpelling (reversibility
//per Rust RFC 2603: the structure defines the spelling, so feeding a
//descriptor's stored fields back in reproduces the same key).
//`classRowRef` carries the class/interface row index for class-typed
//arguments, -1 otherwise.
struct TypeArgDesc
{
	std::string spelling;
	uint8_t resolvedKind = 0;
	int32_t classRowRef = -1;
	std::vector<TypeArgDesc> nested;
};

//The argument's tree-complete canonical spelling: leaves pass through;
//an instantiation argument renders as Name<Arg1,Arg2> built recursively
//from its nested tree. Intern keys and Spelling() go through this —
//never join the raw spelling fields of nested arguments.
std::string CanonicalArgSpelling(const TypeArgDesc& arg);

//Immutable record of one generic instantiation: the template's bare
//name plus the type-argument list. Spelling() is the canonical no-space
//form ("Box<int>", "List<Box<int>>") — the intern and serialization
//key, so it never carries presentation spaces (the diagnostic render
//joins with ", " instead; see ExprResolverGenerics.cpp).
class GenericInstDescriptor
{
public:
	GenericInstDescriptor(std::string templateName,
		std::vector<TypeArgDesc> typeArgs);
	const std::string& TemplateName() const { return m_templateName; }
	const std::string& Spelling() const { return m_spelling; }
	const std::vector<TypeArgDesc>& TypeArgs() const { return m_typeArgs; }
	GenericInstDescriptor(const GenericInstDescriptor&) = delete;
	GenericInstDescriptor& operator=(const GenericInstDescriptor&) = delete;
private:
	std::string m_templateName;
	std::vector<TypeArgDesc> m_typeArgs;
	std::string m_spelling;
};

//Return the interned descriptor for this instantiation, creating it on
//first use; returned pointers are stable for the process lifetime. The
//intern key is the full canonical spelling (reversibility per Rust RFC
//2603: the spelling reconstructs the argument tree). Interning happens
//on the compile and module-load paths, which are serialized within the
//process — no locking here.
GenericInstDescriptor* InternDescriptor(const std::string& templateName,
	const std::vector<TypeArgDesc>& typeArgs);

} //namespace nlang
