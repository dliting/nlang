/*--- GenericInstanceRegistry.cpp — 声明泛型实例注册表的实现：占位符
    供给、实例缓存与按单元分桶（类契约见公共头文件）。
    从 ExprResolverGenericInstantiation.cpp 抽取（文件规模守卫驱动的拆分，零行为
    变化）。
---*/
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnData.h"
#include "ScriptLocation.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <cassert>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

GenericInstanceRegistry::~GenericInstanceRegistry()
{
	//The registry owns every node it created. Instances and placeholders
	//are detached from any tree, so nothing else frees them.
	for (auto& kv : m_instances)
		delete kv.second;
	for (auto& kv : m_ifaceInstances)
		delete kv.second;
	for (auto& kv : m_placeholders)
		delete kv.second;
}

SnField* GenericInstanceRegistry::PlaceholderFor(SyntaxNode& tmpl,
	const std::string& name, size_t position)
{
	const PlaceholderKey key{&tmpl, name};
	auto it = m_placeholders.find(key);
	if (it != m_placeholders.end())
		return it->second;
	//An opaque class-typed node: every type-binding site accepts it and
	//nothing dereferences into it (members never reference placeholders).
	//The node flag marks it for the backend's context-free kind
	//classification (RTK_TypeParam); the index map below remains the
	//substitution-side position lookup.
	auto* pName = new std::string(name);
	auto* pPlaceholder = new SnClassDecl(pName, nullptr,
		new PtrList<SnField>(), *tmpl.Location());
	pPlaceholder->SetTypeParamPlaceholder();
	m_placeholders[key] = pPlaceholder;
	m_placeholderIndex[pPlaceholder] = position;
	return pPlaceholder;
}

bool GenericInstanceRegistry::IsPlaceholder(const SnField* pField) const
{
	return pField && m_placeholderIndex.count(pField) > 0;
}

size_t GenericInstanceRegistry::PlaceholderIndex(
	const SnField* pPlaceholder) const
{
	auto it = m_placeholderIndex.find(pPlaceholder);
	NLANG_ASSERT(it != m_placeholderIndex.end());
	return it->second;
}

SnClassDecl* GenericInstanceRegistry::InstanceOf(
	const SnClassDecl* pInstance) const
{
	auto it = m_instanceTemplate.find(pInstance);
	return it != m_instanceTemplate.end()
		? const_cast<SnClassDecl*>(it->second) : nullptr;
}

SnField* GenericInstanceRegistry::RecordOwnerOfMember(
	const SnField* pMember) const
{
	//Fill-time map lookup (AdoptMemberClone): the pair was recorded
	//when the copy joined the shell, so later member-list mutations on
	//either side cannot drift the mapping. Non-const like InstanceOf:
	//the registry creates and owns these nodes, and record consumers
	//need the owning pointer.
	auto it = m_memberRecordOwner.find(pMember);
	return it != m_memberRecordOwner.end()
		? const_cast<SnField*>(it->second) : nullptr;
}

void GenericInstanceRegistry::AdoptInstance(SnClassDecl* pInstance,
	const SnClassDecl* pTemplate,
	const std::vector<const SnField*>& typeArgs)
{
	InstanceKey<SnClassDecl> key{pTemplate, typeArgs};
	m_instances[key] = pInstance;
	m_instanceTemplate[pInstance] = pTemplate;
	//Node-side mirror of m_instanceTemplate: the visibility folds read it
	//from the record with no registry handle. const_cast like InstanceOf —
	//the registry owns both records.
	pInstance->InstantiatedFrom(const_cast<SnClassDecl*>(pTemplate));
}

void GenericInstanceRegistry::AdoptMemberClone(SnField* pClone,
	const SnField* pSource)
{
	m_memberRecordOwner[pClone] = pSource;
}

SnClassDecl* GenericInstanceRegistry::InstantiationOfClone(
	const SnField* pMember) const
{
	auto it = m_memberInstantiation.find(pMember);
	return it != m_memberInstantiation.end()
		? const_cast<SnClassDecl*>(it->second) : nullptr;
}

void GenericInstanceRegistry::AdoptCloneInstantiation(SnField* pClone,
	const SnClassDecl* pInstance)
{
	m_memberInstantiation[pClone] = pInstance;
}

SnInterfaceDecl* GenericInstanceRegistry::InstantiationOfInterfaceClone(
	const SnField* pMember) const
{
	auto it = m_ifaceCloneInstantiation.find(pMember);
	return it != m_ifaceCloneInstantiation.end()
		? const_cast<SnInterfaceDecl*>(it->second) : nullptr;
}

void GenericInstanceRegistry::AdoptInterfaceCloneInstantiation(
	SnField* pClone, const SnInterfaceDecl* pInstance)
{
	m_ifaceCloneInstantiation[pClone] = pInstance;
}

SnClassDecl* GenericInstanceRegistry::FindInstance(
	const SnClassDecl* pTemplate,
	const std::vector<const SnField*>& typeArgs) const
{
	auto it = m_instances.find(InstanceKey<SnClassDecl>{pTemplate, typeArgs});
	return it != m_instances.end() ? it->second : nullptr;
}

void GenericInstanceRegistry::ForEachInstance(
	const std::function<void(SnClassDecl&)>& visitor) const
{
	for (const auto& kv : m_instances)
		visitor(*kv.second);
}

SnInterfaceDecl* GenericInstanceRegistry::FindInterfaceInstance(
	const SnInterfaceDecl* pTemplate,
	const std::vector<const SnField*>& typeArgs) const
{
	auto it = m_ifaceInstances.find(
		InstanceKey<SnInterfaceDecl>{pTemplate, typeArgs});
	return it != m_ifaceInstances.end() ? it->second : nullptr;
}

void GenericInstanceRegistry::AdoptInterfaceInstance(
	SnInterfaceDecl* pInstance, const SnInterfaceDecl* pTemplate,
	const std::vector<const SnField*>& typeArgs)
{
	InstanceKey<SnInterfaceDecl> key{pTemplate, typeArgs};
	m_ifaceInstances[key] = pInstance;
	m_ifaceInstanceTemplate[pInstance] = pTemplate;
	pInstance->InstantiatedFrom(const_cast<SnInterfaceDecl*>(pTemplate));
}

SnInterfaceDecl* GenericInstanceRegistry::InstanceOfInterface(
	const SnInterfaceDecl* pInstance) const
{
	auto it = m_ifaceInstanceTemplate.find(pInstance);
	return it != m_ifaceInstanceTemplate.end()
		? const_cast<SnInterfaceDecl*>(it->second) : nullptr;
}

} //namespace nlang
