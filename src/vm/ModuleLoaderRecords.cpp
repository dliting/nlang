/*---
    ModuleLoaderRecords.cpp — .ncu 复合记录表解析（functions / structs
    两段），从 ModuleLoader.cpp 逐字搬移（源尺寸守卫拆分，零行为
    变化）。段间顺序、字段序、错误措辞与原实现完全一致；classes 段见
    ModuleLoaderRecordsClasses.cpp（2026-10-22 并发线合并后拆分，一表
    一 TU）。ReadTypeDesc / ReadDefaultValueDesc 是各段共用的流级
    适配器（u16 长度前缀 + TypeDesc.cpp 的字节层解析），0.9.12 (v2.7)
    起经头文件跨 TU 共享给接口段 ModuleLoaderRecordsInterfaces.cpp。
    0.9.12 (v2.7)：结构记录尾部读构造器访问。
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

        //K2 slice 5: eval-area handle mask, the record's trailing field.
        //The length must agree with the frame layout the same record
        //declares — a mismatch is module corruption, not a fallback
        //case (the GC's mask-length fallback only guards in-memory
        //records an embedder built by hand).
        {
            uint32_t maskBytes = 0;
            fs.read(reinterpret_cast<char*>(&maskBytes),
                    sizeof(maskBytes));
            if (!fs.good() || maskBytes > (1u << 16))
                throw std::runtime_error(
                    "Invalid module: bad handle-mask length");
            func.evalAreaHandleMask.resize(maskBytes);
            if (maskBytes > 0)
                fs.read(reinterpret_cast<char*>(
                    func.evalAreaHandleMask.data()), maskBytes);
            if (!fs.good())
                throw std::runtime_error(
                    "Invalid module: truncated handle mask");
            const size_t totalSlots = func.localsSize / kFrameSlotBytes;
            const size_t expected = totalSlots > func.paramCount
                ? (totalSlots - func.paramCount + 3) / 4 : 0;
            if (maskBytes != expected)
                throw std::runtime_error(
                    "Invalid module: handle mask length disagrees with "
                    "frame layout in '" + func.name + "'");
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

} // namespace nlang
