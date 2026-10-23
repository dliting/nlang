/*---
    ModuleSaverRecordsEnums.cpp — .ncu 枚举记录表序列化（enums 段），
    从 ModuleSaver.cpp 分文件（0.10.14 v2.9 源尺寸守卫拆分，一表一
    TU，零行为变化）。读取侧对偶内联在 ModuleLoader::LoadFromBytes 的
    enums 段分支。
---*/
#include "ModuleSaverRecords.h"

namespace nlang {

//Phase 8e-9b: enum name tables (per-enum vector of value names).
//Format: uint32 enumCount, then per enum: uint32 valueCount, then
//per value: uint32 nameLen + name bytes.
void WriteEnumRecords(std::ostream& fs, const CompiledModule& mod)
{
    uint32_t enumCount = static_cast<uint32_t>(mod.enumNames.size());
    fs.write(reinterpret_cast<const char*>(&enumCount), sizeof(enumCount));
    for (auto& names : mod.enumNames) {
        uint32_t valueCount = static_cast<uint32_t>(names.size());
        fs.write(reinterpret_cast<const char*>(&valueCount), sizeof(valueCount));
        for (auto& n : names) {
            uint32_t len = static_cast<uint32_t>(n.size());
            fs.write(reinterpret_cast<const char*>(&len), sizeof(len));
            fs.write(n.c_str(), len);
        }
    }

    //v2.0 phase 6: enum qualified keys, parallel to enumNames (enumCount
    //entries, no separate count — the two vectors are kept in lockstep by
    //their construction sites: RegisterEnums, Import's placeholder push,
    //and EnumSymbolSlot's placeholder push).
    for (auto& key : mod.enumKeys) {
        uint16_t len = static_cast<uint16_t>(key.size());
        fs.write(reinterpret_cast<const char*>(&len), sizeof(len));
        fs.write(key.c_str(), len);
    }

    //v2.9 (0.10.14): per-enum member-method tables, parallel to
    //enumNames/enumKeys (the writer prologue refuses a diverging
    //module, so the rows are guaranteed present). Per enum: uint32
    //methodCount, then per method: uint16 function index + uint8
    //access byte.
    for (uint32_t ei = 0; ei < enumCount; ++ei) {
        const auto& methods = mod.enumMethodIndices[ei];
        uint32_t methodCount = static_cast<uint32_t>(methods.size());
        fs.write(reinterpret_cast<const char*>(&methodCount),
                 sizeof(methodCount));
        for (size_t mi = 0; mi < methods.size(); ++mi) {
            uint16_t idx = methods[mi];
            fs.write(reinterpret_cast<const char*>(&idx), sizeof(idx));
            uint8_t access = mod.enumMethodAccess[ei][mi];
            fs.write(reinterpret_cast<const char*>(&access),
                     sizeof(access));
        }
    }
}

} // namespace nlang
