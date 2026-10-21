/*---
    ModuleSaverRecordsInterfaces.cpp — .ncu 接口记录表序列化（interfaces
    段），从 ModuleSaver.cpp 分文件（0.9.12 v2.7 源尺寸守卫拆分，
    一表一 TU，零行为变化）。共用的流级适配器 WriteLenPrefixedTypeDesc /
    WriteDefaultValueDesc 定义在 ModuleSaver.cpp，经
    ModuleSaverRecords.h 跨 TU 共享；读取侧对偶在
    ModuleLoaderRecordsInterfaces.cpp。
---*/
#include "ModuleSaverRecords.h"

namespace nlang {

//0.9.10 D3: exported-constants block of one interface — u16 count, then
//per constant a u16 name, a length-prefixed TypeDesc and a
//DefaultValueDesc record (the reader is ReadInterfaceConstants).
static void WriteInterfaceConstants(std::ostream& fs,
                                    const CompiledInterface& ci)
{
    uint16_t constantCount = static_cast<uint16_t>(ci.constants.size());
    fs.write(reinterpret_cast<const char*>(&constantCount),
             sizeof(constantCount));
    for (const auto& constant : ci.constants) {
        uint16_t cnameLen = static_cast<uint16_t>(constant.name.size());
        fs.write(reinterpret_cast<const char*>(&cnameLen),
                 sizeof(cnameLen));
        fs.write(constant.name.c_str(), cnameLen);
        WriteLenPrefixedTypeDesc(fs, constant.type);
        WriteDefaultValueDesc(fs, constant.value);
    }
}

//0.9.12 (v2.7): instance-method signature block of one interface —
//u16 count, then per signature a u16 name, u16 formal count, per formal
//a u8 flags + length-prefixed TypeDesc (mirroring the function
//paramTypeDescs block), a length-prefixed return TypeDesc written
//unconditionally (a void method carries the RTK_Void kind byte — the
//producer stamps it; a type descriptor is never zero bytes, so length
//cannot encode void), and u16 default count + DefaultValueDesc records
//(count == formals). The consumer rebuilds bodyless stubs from these to
//resolve calls on imported interface receivers and check implements
//contracts.
static void WriteInterfaceSignatures(std::ostream& fs,
                                     const CompiledInterface& ci)
{
    uint16_t sigCount = static_cast<uint16_t>(ci.instanceMethods.size());
    fs.write(reinterpret_cast<const char*>(&sigCount), sizeof(sigCount));
    for (const auto& sig : ci.instanceMethods) {
        uint16_t snameLen = static_cast<uint16_t>(sig.name.size());
        fs.write(reinterpret_cast<const char*>(&snameLen),
                 sizeof(snameLen));
        fs.write(sig.name.c_str(), snameLen);
        uint16_t paramCount = static_cast<uint16_t>(
            sig.paramTypeDescs.size());
        fs.write(reinterpret_cast<const char*>(&paramCount),
                 sizeof(paramCount));
        for (const auto& ptd : sig.paramTypeDescs) {
            fs.write(reinterpret_cast<const char*>(&ptd.flags),
                     sizeof(ptd.flags));
            WriteLenPrefixedTypeDesc(fs, ptd.type);
        }
        WriteLenPrefixedTypeDesc(fs, sig.returnType);
        uint16_t defaultCount = static_cast<uint16_t>(
            sig.defaultValues.size());
        fs.write(reinterpret_cast<const char*>(&defaultCount),
                 sizeof(defaultCount));
        for (const auto& dv : sig.defaultValues)
            WriteDefaultValueDesc(fs, dv);
    }
}

//0.9.10 v2.4: interface statics table. Format mirrors the class method
//block: u32 count, then per interface the u16-length-prefixed qualified
//key plus its static function indices, the (v2.5, D3) constants block
//and the (v2.7) instance-method signature block.
void WriteInterfaceRecords(std::ostream& fs, const CompiledModule& mod)
{
    uint32_t ifaceCount = static_cast<uint32_t>(mod.interfaces.size());
    fs.write(reinterpret_cast<const char*>(&ifaceCount),
             sizeof(ifaceCount));
    for (const auto& ci : mod.interfaces) {
        uint16_t nameLen = static_cast<uint16_t>(ci.name.size());
        fs.write(reinterpret_cast<const char*>(&nameLen),
                 sizeof(nameLen));
        fs.write(ci.name.c_str(), nameLen);
        uint16_t staticCount = static_cast<uint16_t>(
            ci.staticMethodIndices.size());
        fs.write(reinterpret_cast<const char*>(&staticCount),
                 sizeof(staticCount));
        for (auto idx : ci.staticMethodIndices)
            fs.write(reinterpret_cast<const char*>(&idx), sizeof(idx));
        WriteInterfaceConstants(fs, ci);
        WriteInterfaceSignatures(fs, ci);
    }
}

} // namespace nlang
