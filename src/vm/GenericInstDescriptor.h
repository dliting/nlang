/*--- GenericInstDescriptor.h — 泛型实例化描述符：模板裸名＋类型实参表，
    规范拼写为无空格形Base<Arg1,Arg2>，按拼写intern。字典模型的运行时
    类型实参载体——对象侧并行数组（实例化身份）、转型比较、typeName、
    ABI回调与序列化的共享数据面。类型实参词汇（TypeArgDesc与
    CanonicalArgSpelling）随v0.13.1层界收正迁入编译器侧共享头。
---*/
#pragma once
#include <nlang/compiler/TypeArgSpelling.h>

namespace nlang
{

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

struct CompiledModule;

//Load-side residency: intern the descriptor a canonical spelling names,
//rebuilding its argument tree by parsing (reversibility — the structure
//defines the spelling). Leaf link keys resolve against the linked
//module's struct/class/enum tables so the parsed argument carries the
//runtime kind; an unresolved key stays RTK_Class with rowRef -1
//(display-correct; dispatch paths degrade gracefully). Throws
//std::runtime_error on a non-canonical spelling (corrupt image).
GenericInstDescriptor* InternDescriptorFromSpelling(
	const std::string& spelling, const CompiledModule& module);

} //namespace nlang
