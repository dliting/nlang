/*--- GenericInstanceRegistry.cpp — 声明泛型实例注册表的实现：占位符
    供给、实例缓存与按单元分桶（类契约见公共头文件）。
    从 ExprResolverGenericMint.cpp 抽取（文件规模守卫驱动的拆分，零行为
    变化）。
---*/
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnData.h"
#include "ScriptLocation.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <cassert>

namespace nlang
{

GenericInstanceRegistry::~GenericInstanceRegistry()
{
	//The registry owns every node it minted. Instances and placeholders
	//are detached from any tree, so nothing else frees them.
	for (auto& kv : m_instances)
		delete kv.second;
	for (auto& kv : m_placeholders)
		delete kv.second;
}

SnField* GenericInstanceRegistry::PlaceholderFor(SnClassDecl& tmpl,
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
	assert(it != m_placeholderIndex.end());
	return it->second;
}

bool GenericInstanceRegistry::HasPlaceholderArg(
	const SnClassDecl& inst) const
{
	for (const SnField* pArg : inst.GenericTypeArgs()) {
		if (IsPlaceholder(pArg))
			return true;
		if (pArg->Kind() == NK_ClassDecl) {
			const auto& argInst = static_cast<const SnClassDecl&>(*pArg);
			if (argInst.IsGenericInstantiation()
					&& HasPlaceholderArg(argInst))
				return true;
		}
	}
	return false;
}

SnClassDecl* GenericInstanceRegistry::InstanceOf(
	const SnClassDecl* pInstance) const
{
	auto it = m_instanceTemplate.find(pInstance);
	return it != m_instanceTemplate.end()
		? const_cast<SnClassDecl*>(it->second) : nullptr;
}

void GenericInstanceRegistry::AdoptInstance(SnClassDecl* pInstance,
	const SnClassDecl* pTemplate,
	const std::vector<const SnField*>& typeArgs, uint32_t ownerUnit)
{
	InstanceKey key{pTemplate, typeArgs};
	m_instances[key] = pInstance;
	m_instanceTemplate[pInstance] = pTemplate;
	m_instancesByUnit[ownerUnit].push_back(pInstance);
}

SnClassDecl* GenericInstanceRegistry::FindInstance(
	const SnClassDecl* pTemplate,
	const std::vector<const SnField*>& typeArgs) const
{
	auto it = m_instances.find(InstanceKey{pTemplate, typeArgs});
	return it != m_instances.end() ? it->second : nullptr;
}

void GenericInstanceRegistry::ForEachInstance(uint32_t ownerUnit,
	const std::function<void(SnClassDecl&)>& visitor) const
{
	auto it = m_instancesByUnit.find(ownerUnit);
	if (it == m_instancesByUnit.end())
		return;
	for (auto* pInstance : it->second)
		visitor(*pInstance);
}

} //namespace nlang
