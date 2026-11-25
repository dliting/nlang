/*--- GenericInstDescriptor.h — 泛型实例化描述符：模板非限定名＋类型实参表，
    规范拼写为无空格形Base<Arg1,Arg2>，按拼写intern。字典模型的运行时
    类型实参存放位置——对象侧并行数组（实例化身份）、转型比较、typeName、
    ABI回调与序列化的共享数据面。类型实参词汇（TypeArgDesc与
    CanonicalArgSpelling）随v0.13.1层界收正迁入编译器侧共享头。
---*/
#pragma once
#include <nlang/core/TypeArgSpelling.h>

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

//Canonical no-space join: "Base<Arg1,Arg2>" — the intern and
//serialization key (reversibility per Rust RFC 2603: the spelling
//reconstructs the argument tree). Arguments spell through
//CanonicalArgSpelling so nested instantiations stay distinguishable.
//Shared with the compile backend, which keys descriptor table rows by
//spelling WITHOUT interning — the pool below is runtime-only (see
//InternDescriptor).
std::string CanonicalSpelling(const std::string& templateName,
	const std::vector<TypeArgDesc>& typeArgs);

//Return the interned descriptor for this instantiation, creating it on
//first use; returned pointers are stable for the process lifetime. The
//intern key is the full canonical name (reversibility per Rust RFC
//2603: the spelling reconstructs the argument tree). Interning happens
//only on the runtime paths — module load (InitDescriptorTable) and an
//executor's first instantiation cast or array stamp
//(CastTargetDescriptor) — never during compilation: a compile-phase
//intern would seed the pool with TypeArgDesc rowRefs of the compiling
//module, and a later runtime lookup keyed by the same spelling would
//receive that stale entry instead of resolving against the linked
//module (v0.13.1c finding: ncc compile+run miswalked casts through
//exactly such an entry). These paths are serialized within the process
//today (one executing executor per process — nvm, nide, ndb, the test
//hosts), so the pool carries no lock of its own.
GenericInstDescriptor* InternDescriptor(const std::string& templateName,
	const std::vector<TypeArgDesc>& typeArgs);

struct CompiledModule;

//Load-side residency: intern the descriptor a canonical name names,
//rebuilding its argument tree by parsing (reversibility — the structure
//defines the spelling). Leaf link keys resolve against the linked
//module's struct/class/enum tables so the parsed argument carries the
//runtime kind. 0.13.8 item 8: a dotted head must name a package the
//module knows (its own modulePath, a live import slot's target, or a
//nlink-merged unit's path), and every leaf and nested-template row must
//resolve — anything else throws std::runtime_error (corrupt image), as
//does a non-canonical name.
GenericInstDescriptor* InternDescriptorFromSpelling(
	const std::string& spelling, const CompiledModule& module);

} //namespace nlang
