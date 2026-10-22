/*---
    ModuleLoaderRecordsClasses.cpp — .ncu 类记录表解析（classes 段），
    从 ModuleLoaderRecords.cpp 分文件（2026-10-22 并发线合并后源尺寸
    守卫拆分，一表一 TU，零行为变化）。共用的流级适配器 ReadTypeDesc
    定义在 ModuleLoaderRecords.cpp，经 ModuleLoaderRecords.h 跨 TU 共享。
    0.9.12 (v2.7)：类记录改读自有字段＋方法访问/虚标志＋构造器访问；
    超类链终止/无环/深度校验在读毕统一执行。
---*/
#include "ModuleLoaderRecords.h"
#include <nlang/runtime/NodeConsts.h>
#include <stdexcept>

namespace nlang {

void ReadClassRecords(std::istream& fs, CompiledModule& mod)
{
    uint32_t classCount;
    fs.read(reinterpret_cast<char*>(&classCount), sizeof(classCount));
    if (!fs.good() || classCount > (1u << 24))
        throw std::runtime_error("Invalid module: bad class count");
    mod.classes.resize(classCount);

    for (uint32_t i = 0; i < classCount; ++i) {
        auto& cc = mod.classes[i];

        uint32_t ccNameLen;
        fs.read(reinterpret_cast<char*>(&ccNameLen), sizeof(ccNameLen));
        if (!fs.good() || ccNameLen > (1u << 24))
            throw std::runtime_error("Invalid module: bad class name length");
        cc.name.resize(ccNameLen);
        fs.read(cc.name.data(), ccNameLen);

        fs.read(reinterpret_cast<char*>(&cc.fieldCount),
                sizeof(cc.fieldCount));
        fs.read(reinterpret_cast<char*>(&cc.superClassIdx),
                sizeof(cc.superClassIdx));

        cc.fieldNames.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            uint32_t fnLen;
            fs.read(reinterpret_cast<char*>(&fnLen), sizeof(fnLen));
            if (!fs.good() || fnLen > (1u << 24))
                throw std::runtime_error("Invalid module: bad class field name length");
            cc.fieldNames[j].resize(fnLen);
            fs.read(cc.fieldNames[j].data(), fnLen);
        }

        cc.fieldTypeKinds.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.fieldTypeKinds[j]),
                    sizeof(cc.fieldTypeKinds[j]));
        }

        cc.fieldStructIndices.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.fieldStructIndices[j]),
                    sizeof(cc.fieldStructIndices[j]));
        }

        cc.fieldClassIndices.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.fieldClassIndices[j]),
                    sizeof(cc.fieldClassIndices[j]));
        }

        //v1.12: per-field type descriptors (the gate is gone — the
        //v1.13 floor subsumes 12; see the function-record site).
        {
            cc.fieldTypeDescs.resize(cc.fieldCount);
            for (uint16_t j = 0; j < cc.fieldCount; ++j)
                cc.fieldTypeDescs[j] = ReadTypeDesc(fs);
        }

        cc.fieldAccess.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.fieldAccess[j]),
                    sizeof(cc.fieldAccess[j]));
            //0.9.12 (v2.7): access bytes are the compiler's AccessType
            //values; anything outside {FA_Private..FA_Public} is a
            //corrupt image (the consumer stub rebuilds visibility from
            //them, so a wild value would silently mis-gate members).
            if (!fs.good() || cc.fieldAccess[j] > FA_Public)
                throw std::runtime_error(
                    "Invalid module: bad class field access");
        }

        //Method indices. 0.9.12 (v2.7): the table carries every member
        //function (static included; staticness rides the function
        //record's isStatic flag) — the v2.4 publicStaticMethodIndices
        //block is gone.
        uint16_t methodCount = 0;
        fs.read(reinterpret_cast<char*>(&methodCount), sizeof(methodCount));
        if (!fs.good() || methodCount > (1u << 12))
            throw std::runtime_error(
                "Invalid module: bad class method count");
        cc.methodIndices.resize(methodCount);
        for (uint16_t j = 0; j < methodCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.methodIndices[j]),
                    sizeof(cc.methodIndices[j]));
        }
        if (!fs.good())
            throw std::runtime_error(
                "Invalid module: truncated class method indices");
        //Every entry must address the function table: the consumer
        //stub rebuilds a member per index and nlink's Lookup faults on
        //an out-of-range entry — refusing here gives the precise
        //diagnostic at load time.
        for (auto idx : cc.methodIndices)
            if (idx >= mod.functions.size())
                throw std::runtime_error(
                    "Invalid module: class method index out of range");

        //0.9.12 (v2.7): declared access and virtuality per method,
        //parallel to methodIndices (counts ride on methodCount — the
        //saver writes exactly methodCount bytes of each).
        cc.methodAccess.resize(methodCount);
        for (auto& a : cc.methodAccess) {
            fs.read(reinterpret_cast<char*>(&a), sizeof(a));
            if (!fs.good() || a > FA_Public)
                throw std::runtime_error(
                    "Invalid module: bad class method access");
        }
        cc.methodVirtual.resize(methodCount);
        for (auto& v : cc.methodVirtual) {
            fs.read(reinterpret_cast<char*>(&v), sizeof(v));
            if (!fs.good() || v > 1)
                throw std::runtime_error(
                    "Invalid module: bad class method virtual flag");
        }

        //v2.6: constructor function indices in declaration order (see
        //ModuleSaver) — replaces the single constructor index of v2.5.
        uint16_t ctorCount = 0;
        fs.read(reinterpret_cast<char*>(&ctorCount), sizeof(ctorCount));
        if (!fs.good() || ctorCount > (1u << 12))
            throw std::runtime_error(
                "Invalid module: bad class constructor count");
        cc.constructorIndices.resize(ctorCount);
        for (uint16_t j = 0; j < ctorCount; ++j)
            fs.read(reinterpret_cast<char*>(&cc.constructorIndices[j]),
                    sizeof(uint16_t));
        if (!fs.good())
            throw std::runtime_error(
                "Invalid module: truncated class constructor indices");
        for (auto idx : cc.constructorIndices)
            if (idx >= mod.functions.size())
                throw std::runtime_error(
                    "Invalid module: class constructor index out of range");

        //0.9.12 (v2.7): declared access per ctor, parallel to
        //constructorIndices (count rides on ctorCount).
        cc.constructorAccess.resize(ctorCount);
        for (auto& a : cc.constructorAccess) {
            fs.read(reinterpret_cast<char*>(&a), sizeof(a));
            if (!fs.good() || a > FA_Public)
                throw std::runtime_error(
                    "Invalid module: bad class constructor access");
        }
    }

    //0.9.12 (v2.7): the class records carry OWN fields and join their
    //superclasses through superClassIdx, so a broken chain (dangling
    //index, cycle, or depth beyond the table) would silently corrupt
    //every derived layout — walk it once now that all records are in.
    //The depth bound doubles as the cycle detector: a chain that takes
    //more steps than there are classes must revisit a class.
    for (uint32_t i = 0; i < classCount; ++i) {
        int steps = 0;
        int16_t link = mod.classes[i].superClassIdx;
        while (link != -1) {
            if (link < 0 || static_cast<uint32_t>(link) >= classCount
                || ++steps > static_cast<int>(classCount))
                throw std::runtime_error(
                    "Invalid module: corrupt class superclass chain");
            link = mod.classes[static_cast<size_t>(link)].superClassIdx;
        }
    }
}

} // namespace nlang
