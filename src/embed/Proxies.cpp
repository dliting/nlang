/*---
    Proxies.cpp — 参考访问器与四读写代理（spec §6）。
    分层纪律：vm 桥只抛 std::runtime_error，经 Guarded 翻译为 BadValue
    （BadValue/NLangThrow 原样穿透）；kind/索引的自产校验直接抛
    BadValue。builder 代理写宿主侧存储；物化在 RefFactory.cpp。
---*/
#include "RefValue.h"
#include "CallMarshalling.h"
#include <cstring>
#include <string>
#include <utility>

namespace nlang {

//--- 参考访问器 ------------------------------------------------------------

ArrayProxy Value::asArray() const {
    CheckKind(m_kind, Kind::Array, "asArray");
    return ArrayProxy(m_ref);
}
ListProxy Value::asList() const {
    CheckKind(m_kind, Kind::List, "asList");
    return ListProxy(m_ref);
}
DictProxy Value::asDict() const {
    CheckKind(m_kind, Kind::Dict, "asDict");
    return DictProxy(m_ref);
}
ObjectProxy Value::asObject() const {
    CheckKind(m_kind, Kind::Object, "asObject");
    return ObjectProxy(m_ref);
}

namespace {

//builder 字典键的宿主侧内容等价（键限标量/字符串——参考键无恒等外
//表示，堆内比较属 vm 桥 HostKeysEqual 的职责）
bool BuilderKeyEquals(const Value& a, const Value& b) {
    if (a.kind() != b.kind())
        return false;
    switch (a.kind()) {
    case Value::Kind::Null:   return true;
    case Value::Kind::Int:    return a.asInt() == b.asInt();
    case Value::Kind::Long:   return a.asLong() == b.asLong();
    case Value::Kind::Float:  return a.asFloat() == b.asFloat();
    case Value::Kind::Double: return a.asDouble() == b.asDouble();
    case Value::Kind::Bool:   return a.asBool() == b.asBool();
    case Value::Kind::Char:   return a.asChar() == b.asChar();
    case Value::Kind::String: return a.asString() == b.asString();
    default:
        throw BadValue("builder dict keys must be scalar or string values");
    }
}

//builder 字典键的标量/字符串约束（参考键无恒等外表示，堆内比较属 vm
//桥 HostKeysEqual 的职责）。set 入口前置拒绝，避免「首个参考键可入、
//同种参考键再入即抛」的部分支持不一致（BuilderKeyEquals 只在比对两个
//同种参考键时才抛）。
void ValidateBuilderKey(const Value& key) {
    switch (key.kind()) {
    case Value::Kind::Null:
    case Value::Kind::Int:
    case Value::Kind::Long:
    case Value::Kind::Float:
    case Value::Kind::Double:
    case Value::Kind::Bool:
    case Value::Kind::Char:
    case Value::Kind::String:
        return;
    default:
        throw BadValue("builder dict keys must be scalar or string values");
    }
}

//builder 对存储中键的配对位（-1＝缺席）。存储按 [k0,v0,k1,v1,...]
//成对展开（RefValue.h 不变量）。
int64_t FindBuilderPair(const std::shared_ptr<detail::RefValue>& ref,
                        const Value& key) {
    const auto& el = ref->builderElements;
    for (size_t i = 0; i + 1 < el.size(); i += 2)
        if (BuilderKeyEquals(el[i], key))
            return static_cast<int64_t>(i);
    return -1;
}

}  // namespace

//--- ArrayProxy（定长数组；元素 kind 取自数组类型描述）----------------------

ArrayProxy::ArrayProxy(std::shared_ptr<detail::RefValue> ref)
    : m_ref(std::move(ref)) {}

uint32_t ArrayProxy::size() const {
    return Guarded([&] {
        return m_ref->executor->HostArrayLength(m_ref->heapIdx);
    });
}

Value ArrayProxy::get(int32_t index) const {
    if (index < 0)
        throw BadValue("negative array index");
    return Guarded([&]() {
        VmExecutor& exec = *m_ref->executor;
        uint8_t cell[8] = {};
        exec.HostArrayGetCell(m_ref->heapIdx,
                              static_cast<uint32_t>(index), cell);
        return embed::ValueFromCell(exec, exec.HostArrayElemKind(
                                              m_ref->heapIdx), cell);
    });
}

void ArrayProxy::set(int32_t index, const Value& v) {
    if (index < 0)
        throw BadValue("negative array index");
    Guarded([&] {
        VmExecutor& exec = *m_ref->executor;
        uint8_t cell[8] = {};
        embed::ValueIntoCell(exec, v,
                             exec.HostArrayElemKind(m_ref->heapIdx), cell);
        exec.HostArraySetCell(m_ref->heapIdx,
                              static_cast<uint32_t>(index), cell);
    });
}

//--- ListProxy --------------------------------------------------------------

ListProxy::ListProxy(std::shared_ptr<detail::RefValue> ref)
    : m_ref(std::move(ref)) {}

uint32_t ListProxy::size() const {
    if (m_ref->isBuilder)
        return static_cast<uint32_t>(m_ref->builderElements.size());
    return Guarded([&] {
        return m_ref->executor->HostListSize(m_ref->heapIdx);
    });
}

Value ListProxy::get(int32_t index) const {
    if (index < 0)
        throw BadValue("negative list index");
    if (m_ref->isBuilder) {
        if (static_cast<size_t>(index) >= m_ref->builderElements.size())
            throw BadValue("list index out of bounds");
        return m_ref->builderElements[static_cast<size_t>(index)];
    }
    return Guarded([&]() {
        VmExecutor& exec = *m_ref->executor;
        return detail::RefFactory::FromHeapHandle(
            exec, exec.HostListGet(m_ref->heapIdx,
                                   static_cast<uint32_t>(index)));
    });
}

void ListProxy::set(int32_t index, const Value& v) {
    if (index < 0)
        throw BadValue("negative list index");
    if (m_ref->isBuilder) {
        if (static_cast<size_t>(index) >= m_ref->builderElements.size())
            throw BadValue("list index out of bounds");
        m_ref->builderElements[static_cast<size_t>(index)] = v;
        return;
    }
    Guarded([&] {
        VmExecutor& exec = *m_ref->executor;
        exec.HostListSet(m_ref->heapIdx, static_cast<uint32_t>(index),
                         detail::RefFactory::ToHeapHandle(exec, v));
    });
}

void ListProxy::add(const Value& v) {
    if (m_ref->isBuilder) {
        m_ref->builderElements.push_back(v);
        return;
    }
    Guarded([&] {
        VmExecutor& exec = *m_ref->executor;
        exec.HostListPushBack(m_ref->heapIdx,
                              detail::RefFactory::ToHeapHandle(exec, v));
    });
}

void ListProxy::removeAt(int32_t index) {
    if (index < 0)
        throw BadValue("negative list index");
    if (m_ref->isBuilder) {
        if (static_cast<size_t>(index) >= m_ref->builderElements.size())
            throw BadValue("list index out of bounds");
        m_ref->builderElements.erase(
            m_ref->builderElements.begin() + index);
        return;
    }
    Guarded([&] {
        m_ref->executor->HostListRemoveAt(m_ref->heapIdx,
                                          static_cast<uint32_t>(index));
    });
}

void ListProxy::clear() {
    if (m_ref->isBuilder) {
        m_ref->builderElements.clear();
        return;
    }
    Guarded([&] { m_ref->executor->HostListClear(m_ref->heapIdx); });
}

//--- DictProxy --------------------------------------------------------------

DictProxy::DictProxy(std::shared_ptr<detail::RefValue> ref)
    : m_ref(std::move(ref)) {}

bool DictProxy::containsKey(const Value& key) const {
    if (m_ref->isBuilder)
        return FindBuilderPair(m_ref, key) >= 0;
    return Guarded([&]() {
        VmExecutor& exec = *m_ref->executor;
        const int32_t query =
            detail::RefFactory::ToHeapHandle(exec, key);
        const uint32_t n = exec.HostDictSize(m_ref->heapIdx);
        for (uint32_t i = 0; i < n; ++i) {
            int32_t k = 0, val = 0;
            if (!exec.HostDictEntryGet(m_ref->heapIdx, i, k, val))
                break;
            if (exec.HostKeysEqual(k, query))
                return true;
        }
        return false;
    });
}

bool DictProxy::containsKey(const char* key) const {
    return containsKey(Value(key));
}

Value DictProxy::get(const Value& key) const {
    if (m_ref->isBuilder) {
        const int64_t pair = FindBuilderPair(m_ref, key);
        if (pair < 0)
            return Value();   //缺席＝Null（与脚本 d.get 的默认语义一致）
        return m_ref->builderElements[static_cast<size_t>(pair) + 1];
    }
    return Guarded([&]() {
        VmExecutor& exec = *m_ref->executor;
        const int32_t query =
            detail::RefFactory::ToHeapHandle(exec, key);
        const uint32_t n = exec.HostDictSize(m_ref->heapIdx);
        for (uint32_t i = 0; i < n; ++i) {
            int32_t k = 0, val = 0;
            if (!exec.HostDictEntryGet(m_ref->heapIdx, i, k, val))
                break;
            if (exec.HostKeysEqual(k, query))
                return detail::RefFactory::FromHeapHandle(exec, val);
        }
        return Value();
    });
}

Value DictProxy::get(const char* key) const { return get(Value(key)); }

void DictProxy::set(const Value& key, const Value& v) {
    if (m_ref->isBuilder) {
        const int64_t pair = FindBuilderPair(m_ref, key);
        if (pair >= 0) {
            m_ref->builderElements[static_cast<size_t>(pair) + 1] = v;
            return;
        }
        ValidateBuilderKey(key);
        m_ref->builderElements.push_back(key);
        m_ref->builderElements.push_back(v);
        return;
    }
    Guarded([&] {
        VmExecutor& exec = *m_ref->executor;
        exec.HostDictUpsert(m_ref->heapIdx,
                            detail::RefFactory::ToHeapHandle(exec, key),
                            detail::RefFactory::ToHeapHandle(exec, v));
    });
}

void DictProxy::set(const char* key, const Value& v) {
    set(Value(key), v);
}

void DictProxy::remove(const Value& key) {
    if (m_ref->isBuilder) {
        const int64_t pair = FindBuilderPair(m_ref, key);
        if (pair >= 0) {
            const auto first =
                m_ref->builderElements.begin() + pair;
            m_ref->builderElements.erase(first, first + 2);
        }
        return;
    }
    Guarded([&] {
        VmExecutor& exec = *m_ref->executor;
        exec.HostDictRemove(m_ref->heapIdx,
                            detail::RefFactory::ToHeapHandle(exec, key));
    });
}

void DictProxy::remove(const char* key) { remove(Value(key)); }

void DictProxy::clear() {
    if (m_ref->isBuilder) {
        m_ref->builderElements.clear();
        return;
    }
    Guarded([&] { m_ref->executor->HostDictClear(m_ref->heapIdx); });
}

std::vector<Value> DictProxy::keys() const {
    if (m_ref->isBuilder) {
        std::vector<Value> out;
        const auto& el = m_ref->builderElements;
        for (size_t i = 0; i + 1 < el.size(); i += 2)
            out.push_back(el[i]);
        return out;
    }
    return Guarded([&]() {
        std::vector<Value> out;
        VmExecutor& exec = *m_ref->executor;
        const uint32_t n = exec.HostDictSize(m_ref->heapIdx);
        out.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            int32_t k = 0, val = 0;
            if (!exec.HostDictEntryGet(m_ref->heapIdx, i, k, val))
                break;
            out.push_back(detail::RefFactory::FromHeapHandle(exec, k));
        }
        return out;
    });
}

//--- ObjectProxy ------------------------------------------------------------

ObjectProxy::ObjectProxy(std::shared_ptr<detail::RefValue> ref)
    : m_ref(std::move(ref)) {}

Value ObjectProxy::getField(const char* name) const {
    return getField(std::string(name));
}

Value ObjectProxy::getField(const std::string& name) const {
    if (m_ref->isBuilder)
        throw BadValue("builder values carry no fields");
    return Guarded([&]() {
        VmExecutor& exec = *m_ref->executor;
        uint16_t fieldIdx = 0;
        if (!exec.HostFieldNameToIndex(m_ref->heapIdx, name, fieldIdx))
            throw BadValue("no field named " + name);
        uint8_t cell[8] = {};
        const uint8_t declaredKind =
            exec.HostFieldCellGet(m_ref->heapIdx, fieldIdx, cell);
        return embed::ValueFromCell(exec, declaredKind, cell);
    });
}

void ObjectProxy::setField(const char* name, const Value& v) {
    setField(std::string(name), v);
}

void ObjectProxy::setField(const std::string& name, const Value& v) {
    if (m_ref->isBuilder)
        throw BadValue("builder values carry no fields");
    Guarded([&] {
        VmExecutor& exec = *m_ref->executor;
        uint16_t fieldIdx = 0;
        if (!exec.HostFieldNameToIndex(m_ref->heapIdx, name, fieldIdx))
            throw BadValue("no field named " + name);
        //先读声明 kind（Get 顺带探针），清格后按声明编码整格写入
        uint8_t cell[8] = {};
        const uint8_t declaredKind =
            exec.HostFieldCellGet(m_ref->heapIdx, fieldIdx, cell);
        std::memset(cell, 0, sizeof(cell));
        embed::ValueIntoCell(exec, v, declaredKind, cell);
        exec.HostFieldCellSet(m_ref->heapIdx, fieldIdx, cell);
    });
}

}  // namespace nlang
