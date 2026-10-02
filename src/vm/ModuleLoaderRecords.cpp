/*---
    ModuleLoaderRecords.cpp — .ncu 复合记录表解析（functions / structs /
    classes 三段），从 ModuleLoader.cpp 逐字搬移（源尺寸守卫拆分，零行为
    变化）。段间顺序、字段序、错误措辞与原实现完全一致；ReadTypeDesc 是
    三段共用的流级适配器（u16 长度前缀 + TypeDesc.cpp 的字节层解析）。
---*/
#include "ModuleLoaderRecords.h"
#include <stdexcept>
#include <vector>

namespace nlang {

//v1.12: read one length-prefixed type descriptor (u16 len + wire bytes).
//len==0 means "no descriptor" — the caller keeps the NonSerialized
//default. Throws on a bad length or a malformed wire form.
static TypeDesc ReadTypeDesc(std::istream& fs)
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

        //Option B v1.3: per-formal default-value descriptors.
        uint16_t defaultCount = 0;
        fs.read(reinterpret_cast<char*>(&defaultCount), sizeof(defaultCount));
        if (!fs.good() || defaultCount > 256)
            throw std::runtime_error("Invalid module: bad default count");
        func.defaultValues.resize(defaultCount);
        for (uint16_t j = 0; j < defaultCount; ++j) {
            auto& dv = func.defaultValues[j];
            fs.read(reinterpret_cast<char*>(&dv.tag), sizeof(dv.tag));
            fs.read(reinterpret_cast<char*>(&dv.intValue), sizeof(dv.intValue));
            fs.read(reinterpret_cast<char*>(&dv.floatValue),
                    sizeof(dv.floatValue));
            fs.read(reinterpret_cast<char*>(&dv.stringIdx),
                    sizeof(dv.stringIdx));
        }

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
        }

        //Method indices
        uint16_t methodCount;
        fs.read(reinterpret_cast<char*>(&methodCount), sizeof(methodCount));
        cc.methodIndices.resize(methodCount);
        for (uint16_t j = 0; j < methodCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.methodIndices[j]),
                    sizeof(cc.methodIndices[j]));
        }

        //Constructor index
        fs.read(reinterpret_cast<char*>(&cc.constructorIdx),
                sizeof(cc.constructorIdx));
    }
}

} // namespace nlang
