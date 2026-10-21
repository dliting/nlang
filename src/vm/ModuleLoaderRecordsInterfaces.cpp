/*---
    ModuleLoaderRecordsInterfaces.cpp — .ncu 接口记录表解析（interfaces
    段），从 ModuleLoaderRecords.cpp 分文件（0.9.12 v2.7 源尺寸守卫拆分，
    一表一 TU，零行为变化）。共用的流级适配器 ReadTypeDesc /
    ReadDefaultValueDesc 定义在 ModuleLoaderRecords.cpp，经
    ModuleLoaderRecords.h 跨 TU 共享。
---*/
#include "ModuleLoaderRecords.h"
#include <stdexcept>

namespace nlang {

//0.9.12 (v2.7): sanity ceiling for interface signature formals. The
//compiler caps every declaration at kMaxFuncParams (64, SnData.h); the
//loader mirrors the value so a hostile u16 count cannot force a huge
//allocation before the stream check fires.
static constexpr uint16_t kMaxInterfaceSigParams = 64;

//0.9.10 D3 exported-constant block of one interface: u16 count, then per
//constant a u16 name, a length-prefixed TypeDesc and a DefaultValueDesc
//record (see the saver's per-interface block).
static void ReadInterfaceConstants(std::istream& fs, CompiledInterface& ci)
{
    uint16_t constantCount = 0;
    fs.read(reinterpret_cast<char*>(&constantCount),
            sizeof(constantCount));
    if (!fs.good() || constantCount > (1u << 12))
        throw std::runtime_error(
            "Invalid module: bad interface constant count");
    ci.constants.resize(constantCount);
    for (uint16_t j = 0; j < constantCount; ++j) {
        auto& constant = ci.constants[j];
        uint16_t nameLen = 0;
        fs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (!fs.good() || nameLen > (1u << 16))
            throw std::runtime_error(
                "Invalid module: bad interface constant name length");
        constant.name.resize(nameLen);
        fs.read(constant.name.data(), nameLen);
        constant.type = ReadTypeDesc(fs);
        constant.value = ReadDefaultValueDesc(fs);
    }
}

//Read one instance-method signature record (the wire layout is documented
//on the block reader below). The formal count is capped at the compiler's
//own declaration ceiling, the default count must equal it, and string
//defaults must address the constant pool (the linker remaps those
//indices by pool position).
static void ReadOneInterfaceSignature(std::istream& fs,
                                      CompiledInterfaceMethod& sig,
                                      size_t stringPoolSize)
{
    uint16_t nameLen = 0;
    fs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
    if (!fs.good() || nameLen > (1u << 16))
        throw std::runtime_error(
            "Invalid module: bad interface method name length");
    sig.name.resize(nameLen);
    fs.read(sig.name.data(), nameLen);

    uint16_t paramCount = 0;
    fs.read(reinterpret_cast<char*>(&paramCount), sizeof(paramCount));
    if (!fs.good() || paramCount > kMaxInterfaceSigParams)
        throw std::runtime_error(
            "Invalid module: bad interface method parameter count");
    sig.paramTypeDescs.resize(paramCount);
    for (uint16_t k = 0; k < paramCount; ++k) {
        auto& ptd = sig.paramTypeDescs[k];
        fs.read(reinterpret_cast<char*>(&ptd.flags),
                sizeof(ptd.flags));
        ptd.type = ReadTypeDesc(fs);
    }
    if (!fs.good())
        throw std::runtime_error(
            "Invalid module: truncated interface method parameters");
    sig.returnType = ReadTypeDesc(fs);

    uint16_t defaultCount = 0;
    fs.read(reinterpret_cast<char*>(&defaultCount),
            sizeof(defaultCount));
    if (!fs.good() || defaultCount != paramCount)
        throw std::runtime_error(
            "Invalid module: bad interface method default count");
    sig.defaultValues.resize(defaultCount);
    for (uint16_t k = 0; k < defaultCount; ++k) {
        auto& dv = sig.defaultValues[k];
        dv = ReadDefaultValueDesc(fs);
        if (dv.tag == RTK_String && dv.stringIdx >= stringPoolSize)
            throw std::runtime_error(
                "Invalid module: bad interface default string index");
    }
    //The loader enforces the parallel sizes; the record's own
    //count field mirrors the descriptor vector for consumers.
    sig.paramCount = paramCount;
}

//0.9.12 (v2.7) instance-method signature block of one interface: u16
//count, then per signature a u16 name, u16 formal count, per formal a
//u8 flags + length-prefixed TypeDesc (mirroring the function
//paramTypeDescs block), a length-prefixed return TypeDesc written
//unconditionally (a void method carries the RTK_Void kind byte — the
//producer stamps it; the consumer gates stub return types on that
//kind), and u16 default count + DefaultValueDesc records (see
//the saver's per-interface block).
static void ReadInterfaceSignatures(std::istream& fs, CompiledInterface& ci,
                                    size_t stringPoolSize)
{
    uint16_t sigCount = 0;
    fs.read(reinterpret_cast<char*>(&sigCount), sizeof(sigCount));
    if (!fs.good() || sigCount > (1u << 12))
        throw std::runtime_error(
            "Invalid module: bad interface signature count");
    ci.instanceMethods.resize(sigCount);
    for (uint16_t j = 0; j < sigCount; ++j)
        ReadOneInterfaceSignature(fs, ci.instanceMethods[j], stringPoolSize);
}

//0.9.10 v2.4: interface statics table — per interface a u16-length-
//prefixed qualified key plus its static function indices. Mirrors the
//class method block; the qualified key is the linkage identity (like
//enumKeys). 0.9.12 (v2.7): appends the constants block and the
//instance-method signature block.
void ReadInterfaceRecords(std::istream& fs, CompiledModule& mod)
{
    uint32_t ifaceCount = 0;
    fs.read(reinterpret_cast<char*>(&ifaceCount), sizeof(ifaceCount));
    if (!fs.good() || ifaceCount > (1u << 20))
        throw std::runtime_error("Invalid module: bad interface count");
    mod.interfaces.resize(ifaceCount);

    for (uint32_t i = 0; i < ifaceCount; ++i) {
        auto& ci = mod.interfaces[i];
        uint16_t nameLen = 0;
        fs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (!fs.good() || nameLen > (1u << 16))
            throw std::runtime_error("Invalid module: bad interface name length");
        ci.name.resize(nameLen);
        fs.read(ci.name.data(), nameLen);

        uint16_t staticCount = 0;
        fs.read(reinterpret_cast<char*>(&staticCount), sizeof(staticCount));
        if (!fs.good() || staticCount > (1u << 12))
            throw std::runtime_error(
                "Invalid module: bad interface static method count");
        ci.staticMethodIndices.resize(staticCount);
        for (uint16_t j = 0; j < staticCount; ++j)
            fs.read(reinterpret_cast<char*>(&ci.staticMethodIndices[j]),
                    sizeof(uint16_t));

        //0.9.10 D3: exported constants.
        ReadInterfaceConstants(fs, ci);

        //0.9.12 (v2.7): instance-method signatures (see the helper).
        ReadInterfaceSignatures(fs, ci, mod.stringConstants.size());
    }
    if (!fs.good())
        throw std::runtime_error("Invalid module: truncated interface table");
}

} // namespace nlang
