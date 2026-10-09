/*---
    RefFactory.cpp — 参考值构造与元素装箱工厂（spec §6）。
    有根参考值登记 GC 根；builder 物化＝每次跨越新对象（无缓存）；
    装箱元素解码的窄行折叠与 DecodeScalarCell 规则一致。
---*/
#include "RefValue.h"
#include "CallMarshalling.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <cstring>
#include <string>

namespace nlang {

namespace {

//装箱元素解码：tag 行与 DecodeScalarCell 的窄行折叠规则一致（uint 超
//int32 → Long；byte/short 家族 → Int）。
Value DecodeBoxedElement(VmExecutor& exec, int32_t boxIdx) {
    uint8_t tag = 0;
    int64_t bits = 0;
    if (!exec.HostBoxedRead(boxIdx, tag, bits))
        throw BadValue("malformed boxed element record");
    switch (tag) {
    case RTK_String:
        return Value(exec.StrValCopy(static_cast<int32_t>(bits)));
    case RTK_Int32: case RTK_Byte: case RTK_Short:
    case RTK_UByte: case RTK_UShort:
        return Value(static_cast<int32_t>(bits));
    case RTK_UInt32:
        if (static_cast<uint64_t>(bits) > 2147483647u)
            return Value(bits);   //超 int32 的无符号行 → Long
        return Value(static_cast<int32_t>(bits));
    case RTK_Long: case RTK_ULong:
        return Value(bits);
    case RTK_Float: {
        float f = 0.0f;
        const uint32_t low = static_cast<uint32_t>(bits);
        std::memcpy(&f, &low, sizeof(f));
        return Value(f);
    }
    case RTK_Double: {
        double d = 0.0;
        std::memcpy(&d, &bits, sizeof(d));
        return Value(d);
    }
    case RTK_Bool:
        return Value(bits != 0);
    case RTK_Char:
        return Value(static_cast<char32_t>(bits));
    default:
        throw BadValue("unsupported boxed element tag "
            + std::to_string(tag));
    }
}

//builder 元素按声明 TypeDesc 校验（标量行经 EncodeScalarCell 的严格
//校验复用；参考元素的嵌套 builder 不在此层校——递归物化时已无声明）
void ValidateBuilderElement(const Value& e, const TypeDesc* desc,
                            const char* what) {
    if (!desc || desc->kind == RTK_NonSerialized)
        return;
    const uint8_t k = desc->kind;
    if (k == RTK_String) {
        if (e.kind() != Value::Kind::String
                && e.kind() != Value::Kind::Null)
            throw BadValue(std::string("builder ") + what
                + " needs a String or Null host value");
        return;
    }
    if (embed::IsReferenceKind(k)) {
        switch (e.kind()) {
        case Value::Kind::Null: case Value::Kind::Array:
        case Value::Kind::List: case Value::Kind::Dict:
        case Value::Kind::Object: case Value::Kind::Struct:
        case Value::Kind::Func:
            return;
        default:
            throw BadValue(std::string("builder ") + what
                + " needs a reference host value");
        }
    }
    uint8_t scratch[8] = {};
    embed::EncodeScalarCell(e, k, scratch);   //失配即 BadValue
}

}  // namespace

namespace detail {

Value RefFactory::MakeRooted(VmExecutor& exec, int32_t heapIdx,
                             Value::Kind kind) {
    Value v;
    v.m_kind = kind;
    v.m_ref = std::make_shared<RefValue>();
    v.m_ref->executor = &exec;
    v.m_ref->heapIdx = heapIdx;
    exec.PushHostRoot(RTK_Class, heapIdx);
    return v;
}

Value RefFactory::MakeBuilder(VmExecutor& exec, Value::Kind kind) {
    Value v;
    v.m_kind = kind;
    v.m_ref = std::make_shared<RefValue>();
    v.m_ref->executor = &exec;
    v.m_ref->isBuilder = true;
    return v;
}

bool RefFactory::IsRef(const Value& v) { return v.m_ref != nullptr; }

bool RefFactory::IsBuilder(const Value& v) {
    return v.m_ref && v.m_ref->isBuilder;
}

int32_t RefFactory::HeapIdxOf(const Value& v) { return v.m_ref->heapIdx; }

int32_t RefFactory::ToHeapHandle(VmExecutor& exec, const Value& v) {
    switch (v.kind()) {
    case Value::Kind::Null:
        return 0;   //null 元素＝句柄 0
    case Value::Kind::Int:
        return exec.BoxHostScalar(RTK_Int32, v.asInt());
    case Value::Kind::Long:
        return exec.BoxHostScalar(RTK_Long, v.asLong());
    case Value::Kind::Float: {
        float f = v.asFloat();
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        return exec.BoxHostScalar(RTK_Float,
                                  static_cast<int64_t>(bits));
    }
    case Value::Kind::Double: {
        double d = v.asDouble();
        int64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        return exec.BoxHostScalar(RTK_Double, bits);
    }
    case Value::Kind::Bool:
        return exec.BoxHostScalar(RTK_Bool, v.asBool() ? 1 : 0);
    case Value::Kind::Char:
        return exec.BoxHostScalar(RTK_Char,
                                  static_cast<int64_t>(v.asChar()));
    case Value::Kind::String:
        return exec.BoxHostScalar(RTK_String,
                                  exec.MintHostString(v.asString()));
    default:
        break;
    }
    //参考 kind：句柄直传；嵌套 builder 递归物化（无元素声明可校）
    if (v.m_ref) {
        if (v.m_ref->isBuilder)
            return Materialize(exec, v, nullptr);
        return v.m_ref->heapIdx;
    }
    throw BadValue("unsupported host value kind for container storage");
}

Value RefFactory::FromHeapHandle(VmExecutor& exec, int32_t heapIdx) {
    if (heapIdx == 0)
        return Value();   //null 引用
    const uint8_t slotKind = exec.HostSlotKind(heapIdx);
    if (slotKind == RTK_Boxed)
        return DecodeBoxedElement(exec, heapIdx);
    switch (slotKind) {
    case RTK_Class: {
        //擦除式 List/Dict 实例与用户类实例同为 class 槽——运行时类名
        //甄别宿主 Kind（CompiledClass 名与语言类型名一致）
        const std::string name = exec.HostClassName(heapIdx);
        if (name == kBuiltinListTypeName)
            return MakeRooted(exec, heapIdx, Value::Kind::List);
        if (name == kBuiltinDictTypeName)
            return MakeRooted(exec, heapIdx, Value::Kind::Dict);
        return MakeRooted(exec, heapIdx, Value::Kind::Object);
    }
    case RTK_Array:
        return MakeRooted(exec, heapIdx, Value::Kind::Array);
    case RTK_Struct:
        return MakeRooted(exec, heapIdx, Value::Kind::Struct);
    case RTK_Func:
        return MakeRooted(exec, heapIdx, Value::Kind::Func);
    default:
        throw BadValue("element is not a boxed or reference record");
    }
}

int32_t RefFactory::Materialize(VmExecutor& exec, const Value& v,
                                const TypeDesc* declaredType) {
    const Value::Kind bk = v.kind();
    if (bk != Value::Kind::List && bk != Value::Kind::Dict)
        throw BadValue(
            "only List/Dict builders can cross into reference formals");
    if (declaredType) {
        const uint8_t dk = declaredType->kind;
        if (dk == RTK_List || dk == RTK_Dict) {
            if ((dk == RTK_List) != (bk == Value::Kind::List))
                throw BadValue(dk == RTK_List
                    ? "declared List formal needs a List host value"
                    : "declared Dict formal needs a Dict host value");
        } else if (dk != RTK_NonSerialized) {
            throw BadValue(
                "builder value cannot cross into a non-container formal");
        }
    }
    //元素声明位：List 用 elems[0]；Dict 键/值用 elems[0]/elems[1]；
    //描述符缺席（宽 RTK_Class 形参）＝无约束
    const auto elemDesc = [declaredType](size_t i) -> const TypeDesc* {
        return declaredType && declaredType->elems.size() > i
            ? &declaredType->elems[i] : nullptr;
    };
    const auto& elements = v.m_ref->builderElements;
    if (bk == Value::Kind::List) {
        return Guarded([&]() {
            const int32_t listIdx = exec.AllocHostList();
            for (const Value& e : elements) {
                ValidateBuilderElement(e, elemDesc(0), "element");
                exec.HostListPushBack(listIdx, ToHeapHandle(exec, e));
            }
            return listIdx;
        });
    }
    return Guarded([&]() {
        const int32_t dictIdx = exec.AllocHostDict();
        for (size_t i = 0; i + 1 < elements.size(); i += 2) {
            ValidateBuilderElement(elements[i], elemDesc(0), "key");
            ValidateBuilderElement(elements[i + 1], elemDesc(1), "value");
            exec.HostDictUpsert(dictIdx, ToHeapHandle(exec, elements[i]),
                                ToHeapHandle(exec, elements[i + 1]));
        }
        return dictIdx;
    });
}

}  // namespace detail
}  // namespace nlang
