/*---
ModuleSaver.cpp — .nmod serialization (write side).

Extracted from VmBackend::SaveModule in the Phase 10 audit round-7 so the
format has one writer shared by the compiler backend and unit tests: the
old test_module_save_load hand-wrote a v1.0 byte layout that drifted from
the reader and got rejected by the version floor. VmBackend::SaveModule
now only resolves the output path and delegates here. The reader side is
ModuleLoader.cpp — keep the two in lockstep via NMOD_FORMAT_* in
CompiledModule.h.
---*/
#include "nlang/vm/CompiledModule.h"

namespace nlang {

bool WriteCompiledModule(std::ostream& fs, const CompiledModule& mod) {
    if (!fs.good()) return false;

    // Magic
    const char magic[] = "NLANGMOD";
    fs.write(magic, 8);

    // Version (shared with ModuleLoader via CompiledModule.h — single source)
    uint16_t majorVer = NMOD_FORMAT_MAJOR, minorVer = NMOD_FORMAT_MINOR;
    fs.write(reinterpret_cast<const char*>(&majorVer), sizeof(majorVer));
    fs.write(reinterpret_cast<const char*>(&minorVer), sizeof(minorVer));

    // Module name
    uint32_t nameLen = static_cast<uint32_t>(mod.name.size());
    fs.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
    fs.write(mod.name.c_str(), nameLen);

    // String constants
    uint32_t strCount = static_cast<uint32_t>(mod.stringConstants.size());
    fs.write(reinterpret_cast<const char*>(&strCount), sizeof(strCount));
    for (auto& s : mod.stringConstants) {
        uint32_t len = static_cast<uint32_t>(s.size());
        fs.write(reinterpret_cast<const char*>(&len), sizeof(len));
        fs.write(s.c_str(), len);
    }

    // Functions
    uint32_t funcCount = static_cast<uint32_t>(mod.functions.size());
    fs.write(reinterpret_cast<const char*>(&funcCount), sizeof(funcCount));

    for (auto& func : mod.functions) {
        uint32_t fnameLen = static_cast<uint32_t>(func.name.size());
        fs.write(reinterpret_cast<const char*>(&fnameLen), sizeof(fnameLen));
        fs.write(func.name.c_str(), fnameLen);

        fs.write(reinterpret_cast<const char*>(&func.localsSize),
                 sizeof(func.localsSize));
        fs.write(reinterpret_cast<const char*>(&func.paramCount),
                 sizeof(func.paramCount));
        fs.write(reinterpret_cast<const char*>(&func.returnTypeKind),
                 sizeof(func.returnTypeKind));
        fs.write(reinterpret_cast<const char*>(&func.intrinsicId),
                 sizeof(func.intrinsicId));

        //Phase 9f v1.6: native function flag (body-less declaration
        //dispatched through the host's native table by name).
        uint8_t nativeFlag = func.isNative ? 1 : 0;
        fs.write(reinterpret_cast<const char*>(&nativeFlag),
                 sizeof(nativeFlag));

        //Option B v1.3: per-formal default-value descriptors. Always
        //emitted (count first) so reader can skip even when no defaults.
        //Count == func.defaultValues.size(); formals without defaults
        //carry tag=RTK_Void to preserve positional alignment, so the
        //vector is naturally dense.
        uint16_t defaultCount = static_cast<uint16_t>(
            func.defaultValues.size());
        fs.write(reinterpret_cast<const char*>(&defaultCount),
                 sizeof(defaultCount));
        for (const auto& dv : func.defaultValues) {
            fs.write(reinterpret_cast<const char*>(&dv.tag), sizeof(dv.tag));
            fs.write(reinterpret_cast<const char*>(&dv.intValue),
                     sizeof(dv.intValue));
            fs.write(reinterpret_cast<const char*>(&dv.floatValue),
                     sizeof(dv.floatValue));
            fs.write(reinterpret_cast<const char*>(&dv.stringIdx),
                     sizeof(dv.stringIdx));
        }

        uint32_t bcSize = static_cast<uint32_t>(func.bytecode.size());
        fs.write(reinterpret_cast<const char*>(&bcSize), sizeof(bcSize));
        if (bcSize > 0)
            fs.write(reinterpret_cast<const char*>(func.bytecode.data()),
                     bcSize);

        //Phase 9d v1.4: try/catch table. Always emit count first so the
        //reader can skip even when empty. Each entry is 5 uint16 fields.
        uint16_t tryBlockCount = static_cast<uint16_t>(func.tryBlocks.size());
        fs.write(reinterpret_cast<const char*>(&tryBlockCount),
                 sizeof(tryBlockCount));
        for (const auto& tb : func.tryBlocks) {
            fs.write(reinterpret_cast<const char*>(&tb.startPc),
                     sizeof(tb.startPc));
            fs.write(reinterpret_cast<const char*>(&tb.endPc),
                     sizeof(tb.endPc));
            fs.write(reinterpret_cast<const char*>(&tb.handlerPc),
                     sizeof(tb.handlerPc));
            fs.write(reinterpret_cast<const char*>(&tb.exceptionClassIdx),
                     sizeof(tb.exceptionClassIdx));
            fs.write(reinterpret_cast<const char*>(&tb.catchLocalOff),
                     sizeof(tb.catchLocalOff));
        }

        //v1.5: local-variable descriptors. The GC root scan (MarkPhase)
        //walks every frame's locals to find heap references; without this
        //table the loaded module's root set is empty and every collection
        //sweeps live objects. Always emit count first so readers can skip
        //when empty. Names are kept for runtime diagnostics.
        uint16_t localCount = static_cast<uint16_t>(func.locals.size());
        fs.write(reinterpret_cast<const char*>(&localCount),
                 sizeof(localCount));
        for (const auto& ld : func.locals) {
            fs.write(reinterpret_cast<const char*>(&ld.offset),
                     sizeof(ld.offset));
            fs.write(reinterpret_cast<const char*>(&ld.size),
                     sizeof(ld.size));
            fs.write(reinterpret_cast<const char*>(&ld.isParam),
                     sizeof(ld.isParam));
            fs.write(reinterpret_cast<const char*>(&ld.typeKind),
                     sizeof(ld.typeKind));
            uint32_t lnameLen = static_cast<uint32_t>(ld.name.size());
            fs.write(reinterpret_cast<const char*>(&lnameLen),
                     sizeof(lnameLen));
            fs.write(ld.name.c_str(), lnameLen);
        }
    }

    // Struct descriptors
    uint32_t structCount = static_cast<uint32_t>(mod.structs.size());
    fs.write(reinterpret_cast<const char*>(&structCount), sizeof(structCount));

    for (auto& st : mod.structs) {
        uint32_t stNameLen = static_cast<uint32_t>(st.name.size());
        fs.write(reinterpret_cast<const char*>(&stNameLen), sizeof(stNameLen));
        fs.write(st.name.c_str(), stNameLen);

        fs.write(reinterpret_cast<const char*>(&st.fieldCount),
                 sizeof(st.fieldCount));

        //Field names
        for (size_t i = 0; i < st.fieldCount; ++i) {
            uint32_t fnLen = static_cast<uint32_t>(st.fieldNames[i].size());
            fs.write(reinterpret_cast<const char*>(&fnLen), sizeof(fnLen));
            fs.write(st.fieldNames[i].c_str(), fnLen);
        }

        //Field type kinds
        for (size_t i = 0; i < st.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&st.fieldTypeKinds[i]),
                     sizeof(st.fieldTypeKinds[i]));
        }

        //Field struct indices
        for (size_t i = 0; i < st.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&st.fieldStructIndices[i]),
                     sizeof(st.fieldStructIndices[i]));
        }

        //Field class indices
        for (size_t i = 0; i < st.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&st.fieldClassIndices[i]),
                     sizeof(st.fieldClassIndices[i]));
        }
    }

    // Class descriptors
    uint32_t classCount = static_cast<uint32_t>(mod.classes.size());
    fs.write(reinterpret_cast<const char*>(&classCount), sizeof(classCount));

    for (auto& cc : mod.classes) {
        uint32_t nameLen = static_cast<uint32_t>(cc.name.size());
        fs.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        fs.write(cc.name.c_str(), nameLen);

        fs.write(reinterpret_cast<const char*>(&cc.fieldCount),
                 sizeof(cc.fieldCount));
        fs.write(reinterpret_cast<const char*>(&cc.superClassIdx),
                 sizeof(cc.superClassIdx));

        //Field names
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            uint32_t fnLen = static_cast<uint32_t>(cc.fieldNames[i].size());
            fs.write(reinterpret_cast<const char*>(&fnLen), sizeof(fnLen));
            fs.write(cc.fieldNames[i].c_str(), fnLen);
        }

        //Field type kinds
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.fieldTypeKinds[i]),
                     sizeof(cc.fieldTypeKinds[i]));
        }

        //Field struct indices
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.fieldStructIndices[i]),
                     sizeof(cc.fieldStructIndices[i]));
        }

        //Field class indices
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.fieldClassIndices[i]),
                     sizeof(cc.fieldClassIndices[i]));
        }

        //Field access
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.fieldAccess[i]),
                     sizeof(cc.fieldAccess[i]));
        }

        //Method indices
        uint16_t methodCount = static_cast<uint16_t>(cc.methodIndices.size());
        fs.write(reinterpret_cast<const char*>(&methodCount), sizeof(methodCount));
        for (size_t i = 0; i < cc.methodIndices.size(); ++i) {
            fs.write(reinterpret_cast<const char*>(&cc.methodIndices[i]),
                     sizeof(cc.methodIndices[i]));
        }

        //Constructor index
        fs.write(reinterpret_cast<const char*>(&cc.constructorIdx),
                 sizeof(cc.constructorIdx));
    }

    //Array type descriptors
    uint32_t arrayTypeCount = static_cast<uint32_t>(mod.arrayTypes.size());
    fs.write(reinterpret_cast<const char*>(&arrayTypeCount),
             sizeof(arrayTypeCount));
    for (auto& at : mod.arrayTypes) {
        fs.write(reinterpret_cast<const char*>(&at.elemKind),
                 sizeof(at.elemKind));
        fs.write(reinterpret_cast<const char*>(&at.elemTypeIdx),
                 sizeof(at.elemTypeIdx));
    }

    //Phase 8e-9b: enum name tables (per-enum vector of value names).
    //Format: uint32 enumCount, then per enum: uint32 valueCount, then
    //per value: uint32 nameLen + name bytes.
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

    return fs.good();
}

} // namespace nlang
