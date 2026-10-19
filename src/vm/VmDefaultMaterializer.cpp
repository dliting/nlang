/*---
    VmDefaultMaterializer.cpp — DefaultValueDesc 折叠值的唯一落槽位点：
    宿主桥缺省实参填充（VmExecutorHostBridge.cpp::FillOneDefault）与
    struct 字段默认值的分配期物化（VmExecutorAlloc.cpp::AllocStructOnHeap）
    共用（0.9.11 T4，D4）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//Writes one folded constant default into an 8-byte cell — a frame slot or
//a heap field slot; both are 8 bytes wide and zero-initialized, so a
//4-byte write leaves the upper half clean (the record fill is the zero
//source; see the freelist assign in VmExecutorAlloc.cpp). The tag set
//mirrors VmBackend::ExtractDefaultValue's fold surface. True = value
//written. False = nothing to write: RTK_Void is the "no default"
//sentinel, and RTK_Expr/RTK_Unfoldable are function-default-only tags
//(v2.3 rebuilds those as AST at the declaration side, never folded) —
//the caller decides what a missing value means (allocation keeps the
//zero fill, the embedding entry throws). Any other tag is malformed
//module data.
bool VmExecutor::MaterializeDefaultValue(const DefaultValueDesc& d,
                                         uint8_t* cell) {
    switch (d.tag) {
    case RTK_Int32:
    case RTK_Bool:
    case RTK_Char: {
        //4-byte integer-family carriers (Types.cpp ExtractLiteralDefault):
        //bool/char literals land in the int32 channel, negatives in uint32 bits.
        const int32_t v = static_cast<int32_t>(d.intValue);
        std::memcpy(cell, &v, sizeof(v));
        return true;
    }
    case RTK_Long:
    case RTK_ULong:
        std::memcpy(cell, &d.longValue, sizeof(d.longValue));
        return true;
    case RTK_Float:
        std::memcpy(cell, &d.floatValue, sizeof(d.floatValue));
        return true;
    case RTK_Double:
        std::memcpy(cell, &d.doubleValue, sizeof(d.doubleValue));
        return true;
    case RTK_Null: {
        //null binds every reference slot as handle 0, the same sentinel
        //ordinary declarations grant.
        const int32_t nullHandle = 0;
        std::memcpy(cell, &nullHandle, sizeof(nullHandle));
        return true;
    }
    case RTK_String: {
        //Post-link the index is remapped into this module's table; mint a
        //runtime string so the value behaves like any other (AllocStringObj
        //raises the GC-pending flag).
        if (d.stringIdx >= m_currModule->stringConstants.size())
            throw std::runtime_error(
                "NLang VM: default string index out of range");
        const int32_t handle =
            MintNewString(m_currModule->stringConstants[d.stringIdx]);
        std::memcpy(cell, &handle, sizeof(handle));
        return true;
    }
    case RTK_Void:
    case RTK_Expr:
    case RTK_Unfoldable:
        return false;
    default:
        throw std::runtime_error(
            "NLang VM: unknown default value tag in module");
    }
}

} //namespace nlang
