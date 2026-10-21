/*---
    ModuleLoaderRecords.cpp — .ncu 复合记录表解析（functions / structs /
    classes 三段），从 ModuleLoader.cpp 逐字搬移（源尺寸守卫拆分，零行为
    变化）。段间顺序、字段序、错误措辞与原实现完全一致；ReadTypeDesc /
    ReadDefaultValueDesc 是各段共用的流级适配器（u16 长度前缀 +
    TypeDesc.cpp 的字节层解析），0.9.12 (v2.7) 起经头文件跨 TU 共享给
    接口段 ModuleLoaderRecordsInterfaces.cpp（一表一 TU）。
    0.9.12 (v2.7)：类记录改读自有字段＋方法访问/虚标志＋构造器访问
    （publicStatic 块删除）；结构记录尾部读构造器访问；超类链终止/无环/
    深度校验在读毕统一执行。
---*/
#include "ModuleLoaderRecords.h"
#include <nlang/vm/ExprDesc.h>
#include <nlang/runtime/NodeConsts.h>
#include <stdexcept>
#include <vector>

namespace nlang {

//v1.12: read one length-prefixed type descriptor (u16 len + wire bytes).
//len==0 means "no descriptor" — the caller keeps the NonSerialized
//default. Throws on a bad length or a malformed wire form.
TypeDesc ReadTypeDesc(std::istream& fs)
{
    uint16_t len = 0;
    fs.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!fs.good() || len > kMaxTypeDescBytes)
        throw std::runtime_error("Invalid module: bad type descriptor length");
    if (len == 0)
        return TypeDesc{};
    std::vector<uint8_t> bytes(len);
    fs.read(reinterpret_cast<char*>(bytes.data()), len);
    if (!fs.good())
        throw std::runtime_error(
            "Invalid module: truncated type descriptor");
    return ParseTypeDescBytes(bytes.data(), bytes.size());
}

//Option B v1.3 fixed default-value record (see the saver's
//WriteDefaultValueDesc for the layout). Shared by the formal-default
//block, the struct field-default block and (via the header) the
//interface blocks in ModuleLoaderRecordsInterfaces.cpp.
DefaultValueDesc ReadDefaultValueDesc(std::istream& fs)
{
    DefaultValueDesc dv;
    fs.read(reinterpret_cast<char*>(&dv.tag), sizeof(dv.tag));
    fs.read(reinterpret_cast<char*>(&dv.intValue), sizeof(dv.intValue));
    fs.read(reinterpret_cast<char*>(&dv.floatValue),
            sizeof(dv.floatValue));
    fs.read(reinterpret_cast<char*>(&dv.stringIdx),
            sizeof(dv.stringIdx));
    fs.read(reinterpret_cast<char*>(&dv.longValue),
            sizeof(dv.longValue));
    fs.read(reinterpret_cast<char*>(&dv.doubleValue),
            sizeof(dv.doubleValue));
    uint16_t exprLen = 0;
    fs.read(reinterpret_cast<char*>(&exprLen), sizeof(exprLen));
    if (!fs.good() || exprLen > kMaxExprDescBytes)
        throw std::runtime_error(
            "Invalid module: bad default expression length");
    dv.exprBytes.resize(exprLen);
    if (exprLen > 0)
        fs.read(reinterpret_cast<char*>(dv.exprBytes.data()), exprLen);
    if (!fs.good())
        throw std::runtime_error(
            "Invalid module: truncated default expression");
    return dv;
}

void ReadFunctionRecords(std::istream& fs, CompiledModule& mod)
{
    uint32_t funcCount = 0;
    fs.read(reinterpret_cast<char*>(&funcCount), sizeof(funcCount));
    if (!fs.good() || funcCount > (1u << 24))
        throw std::runtime_error("Invalid module: bad function count");
    mod.functions.resize(funcCount);

    for (uint32_t i = 0; i < funcCount; ++i) {
        auto& func = mod.functions[i];

        uint32_t fnameLen;
        fs.read(reinterpret_cast<char*>(&fnameLen), sizeof(fnameLen));
        if (!fs.good() || fnameLen > (1u << 24))
            throw std::runtime_error("Invalid module: bad function name length");
        func.name.resize(fnameLen);
        fs.read(func.name.data(), fnameLen);

        fs.read(reinterpret_cast<char*>(&func.localsSize),
                sizeof(func.localsSize));
        fs.read(reinterpret_cast<char*>(&func.paramCount),
                sizeof(func.paramCount));
        fs.read(reinterpret_cast<char*>(&func.returnTypeKind),
                sizeof(func.returnTypeKind));
        fs.read(reinterpret_cast<char*>(&func.intrinsicId),
                sizeof(func.intrinsicId));
        uint8_t nativeFlag = 0;
        fs.read(reinterpret_cast<char*>(&nativeFlag), sizeof(nativeFlag));
        func.isNative = (nativeFlag != 0);
        //0.9.10 v2.4: static-member flag (see ModuleSaver).
        uint8_t staticFlag = 0;
        fs.read(reinterpret_cast<char*>(&staticFlag), sizeof(staticFlag));
        func.isStatic = (staticFlag != 0);

        //Option B v1.3: per-formal default-value descriptors.
        uint16_t defaultCount = 0;
        fs.read(reinterpret_cast<char*>(&defaultCount), sizeof(defaultCount));
        if (!fs.good() || defaultCount > 256)
            throw std::runtime_error("Invalid module: bad default count");
        func.defaultValues.resize(defaultCount);
        //The record layout lives in ReadDefaultValueDesc (shared with
        //the interface-constant block below).
        for (uint16_t j = 0; j < defaultCount; ++j)
            func.defaultValues[j] = ReadDefaultValueDesc(fs);

        uint32_t bcSize;
        fs.read(reinterpret_cast<char*>(&bcSize), sizeof(bcSize));
        if (!fs.good() || bcSize > (1u << 26))
            throw std::runtime_error("Invalid module: bad bytecode size");
        func.bytecode.resize(bcSize);
        if (bcSize > 0)
            fs.read(reinterpret_cast<char*>(func.bytecode.data()), bcSize);

        //Phase 9d v1.4: try/catch table.
        uint16_t tryBlockCount = 0;
        fs.read(reinterpret_cast<char*>(&tryBlockCount), sizeof(tryBlockCount));
        if (!fs.good() || tryBlockCount > 1024)
            throw std::runtime_error("Invalid module: bad tryBlock count");
        func.tryBlocks.resize(tryBlockCount);
        for (uint16_t j = 0; j < tryBlockCount; ++j) {
            auto& tb = func.tryBlocks[j];
            fs.read(reinterpret_cast<char*>(&tb.startPc), sizeof(tb.startPc));
            fs.read(reinterpret_cast<char*>(&tb.endPc), sizeof(tb.endPc));
            fs.read(reinterpret_cast<char*>(&tb.handlerPc), sizeof(tb.handlerPc));
            fs.read(reinterpret_cast<char*>(&tb.exceptionClassIdx),
                    sizeof(tb.exceptionClassIdx));
            fs.read(reinterpret_cast<char*>(&tb.catchLocalOff),
                    sizeof(tb.catchLocalOff));
        }

        //v1.5: local-variable descriptors (GC root scan, see VmBackend
        //writer side).
        {
            uint16_t localCount = 0;
            fs.read(reinterpret_cast<char*>(&localCount),
                    sizeof(localCount));
            if (!fs.good() || localCount > 4096)
                throw std::runtime_error("Invalid module: bad local count");
            func.locals.resize(localCount);
            for (uint16_t j = 0; j < localCount; ++j) {
                auto& ld = func.locals[j];
                fs.read(reinterpret_cast<char*>(&ld.offset),
                        sizeof(ld.offset));
                fs.read(reinterpret_cast<char*>(&ld.size),
                        sizeof(ld.size));
                fs.read(reinterpret_cast<char*>(&ld.isParam),
                        sizeof(ld.isParam));
                fs.read(reinterpret_cast<char*>(&ld.typeKind),
                        sizeof(ld.typeKind));
                //v1.14: declaration PC for debugger scope visibility
                //(the loader floor is already 14 — read unconditionally
                //like the v1.13 default-value channels above).
                fs.read(reinterpret_cast<char*>(&ld.declPc),
                        sizeof(ld.declPc));
                uint32_t lnameLen = 0;
                fs.read(reinterpret_cast<char*>(&lnameLen),
                        sizeof(lnameLen));
                if (!fs.good() || lnameLen > (1u << 16))
                    throw std::runtime_error("Invalid module: bad local name length");
                ld.name.resize(lnameLen);
                fs.read(ld.name.data(), lnameLen);
            }
        }

        //v1.9 (debugger): per-function source file path.
        {
            uint32_t sfileLen = 0;
            fs.read(reinterpret_cast<char*>(&sfileLen), sizeof(sfileLen));
            if (!fs.good() || sfileLen > (1u << 16))
                throw std::runtime_error(
                    "Invalid module: bad source file length");
            func.sourceFile.resize(sfileLen);
            fs.read(func.sourceFile.data(), sfileLen);
        }

        //v1.12: true formal / return type descriptors. The floor moved
        //past 12 in v1.13, so the gate is gone — the record position is
        //what documents the layout for readers diffing versions.
        {
            uint16_t paramDescCount = 0;
            fs.read(reinterpret_cast<char*>(&paramDescCount),
                    sizeof(paramDescCount));
            if (!fs.good() || paramDescCount > kMaxParamDescCount)
                throw std::runtime_error(
                    "Invalid module: bad param descriptor count");
            func.paramTypeDescs.resize(paramDescCount);
            for (uint16_t j = 0; j < paramDescCount; ++j) {
                auto& ptd = func.paramTypeDescs[j];
                fs.read(reinterpret_cast<char*>(&ptd.flags),
                        sizeof(ptd.flags));
                ptd.type = ReadTypeDesc(fs);
            }
            if (func.returnTypeKind != RTK_Void)
                func.returnTypeDesc = ReadTypeDesc(fs);
        }
    }
}

void ReadStructRecords(std::istream& fs, CompiledModule& mod)
{
    uint32_t structCount;
    fs.read(reinterpret_cast<char*>(&structCount), sizeof(structCount));
    if (!fs.good() || structCount > (1u << 24))
        throw std::runtime_error("Invalid module: bad struct count");
    mod.structs.resize(structCount);

    for (uint32_t i = 0; i < structCount; ++i) {
        auto& st = mod.structs[i];

        uint32_t stNameLen;
        fs.read(reinterpret_cast<char*>(&stNameLen), sizeof(stNameLen));
        if (!fs.good() || stNameLen > (1u << 24))
            throw std::runtime_error("Invalid module: bad struct name length");
        st.name.resize(stNameLen);
        fs.read(st.name.data(), stNameLen);

        fs.read(reinterpret_cast<char*>(&st.fieldCount),
                sizeof(st.fieldCount));

        st.fieldNames.resize(st.fieldCount);
        for (uint16_t j = 0; j < st.fieldCount; ++j) {
            uint32_t fnLen;
            fs.read(reinterpret_cast<char*>(&fnLen), sizeof(fnLen));
            if (!fs.good() || fnLen > (1u << 24))
                throw std::runtime_error("Invalid module: bad struct field name length");
            st.fieldNames[j].resize(fnLen);
            fs.read(st.fieldNames[j].data(), fnLen);
        }

        st.fieldTypeKinds.resize(st.fieldCount);
        for (uint16_t j = 0; j < st.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&st.fieldTypeKinds[j]),
                    sizeof(st.fieldTypeKinds[j]));
        }

        st.fieldStructIndices.resize(st.fieldCount);
        for (uint16_t j = 0; j < st.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&st.fieldStructIndices[j]),
                    sizeof(st.fieldStructIndices[j]));
        }

        st.fieldClassIndices.resize(st.fieldCount);
        for (uint16_t j = 0; j < st.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&st.fieldClassIndices[j]),
                    sizeof(st.fieldClassIndices[j]));
        }

        //v1.12: per-field type descriptors (the gate is gone — the
        //v1.13 floor subsumes 12; see the function-record site).
        {
            st.fieldTypeDescs.resize(st.fieldCount);
            for (uint16_t j = 0; j < st.fieldCount; ++j)
                st.fieldTypeDescs[j] = ReadTypeDesc(fs);
        }

        //v2.6: constructor function indices in declaration order (see
        //ModuleSaver). The v2.6 floor guarantees the block; the count cap
        //is the usual corrupt-image guard.
        uint16_t ctorCount = 0;
        fs.read(reinterpret_cast<char*>(&ctorCount), sizeof(ctorCount));
        if (!fs.good() || ctorCount > (1u << 12))
            throw std::runtime_error(
                "Invalid module: bad struct constructor count");
        st.constructorIndices.resize(ctorCount);
        for (uint16_t j = 0; j < ctorCount; ++j)
            fs.read(reinterpret_cast<char*>(&st.constructorIndices[j]),
                    sizeof(uint16_t));
        if (!fs.good())
            throw std::runtime_error(
                "Invalid module: truncated struct constructor indices");
        //0.9.12 (v2.7): same function-table bound as the class ctor
        //indices — the consumer stub rebuilds a ctor per entry.
        for (auto idx : st.constructorIndices)
            if (idx >= mod.functions.size())
                throw std::runtime_error(
                    "Invalid module: struct constructor index out of range");

        //v2.6: per-field compile-time defaults, dense (RTK_Void = no
        //default). The count must equal fieldCount — a mismatch means the
        //record was written by a different layout and every byte after
        //this block would misparse, so refuse here.
        uint16_t defCount = 0;
        fs.read(reinterpret_cast<char*>(&defCount), sizeof(defCount));
        if (!fs.good() || defCount != st.fieldCount)
            throw std::runtime_error(
                "Invalid module: bad struct field default count");
        st.fieldDefaults.resize(defCount);
        for (uint16_t j = 0; j < defCount; ++j)
            st.fieldDefaults[j] = ReadDefaultValueDesc(fs);

        //0.9.12 (v2.7): declared access per ctor, parallel to
        //constructorIndices (count rides on ctorCount) — the consumer
        //stub rebuilds each struct ctor with its visibility.
        st.constructorAccess.resize(ctorCount);
        for (auto& a : st.constructorAccess) {
            fs.read(reinterpret_cast<char*>(&a), sizeof(a));
            if (!fs.good() || a > FA_Public)
                throw std::runtime_error(
                    "Invalid module: bad struct constructor access");
        }
    }
}

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
