/*---
ModuleSaver.cpp — .ncu serialization (write side).

Extracted from the old VmBackend save step in the Phase 10 audit round-7
so the format has one writer shared by the compiler backend and unit
tests: the old test_module_save_load hand-wrote a v1.0 byte layout that
drifted from the reader and got rejected by the version floor. The
artifact writers (VmBackend::WriteModuleArtifact / the .npkg packer)
resolve the output path and delegate here. The reader side is
ModuleLoader.cpp — keep the two in lockstep via NCU_FORMAT_* in
CompiledModule.h.
---*/
#include "nlang/vm/CompiledModule.h"
#include "ModuleSaverRecords.h"

namespace nlang {

//v1.12: one length-prefixed type descriptor (u16 len + wire bytes).
//len==0 means "no descriptor" — the reader keeps the NonSerialized
//default. Shared by the param / return / per-field sites below and (via
//ModuleSaverRecords.h) by the interface record TU.
void WriteLenPrefixedTypeDesc(std::ostream& fs, const TypeDesc& td)
{
    std::vector<uint8_t> bytes;
    AppendTypeDescBytes(bytes, td);
    //The depth cap bounds a descriptor well under 64 KiB (worst case a
    //full binary Dict tree at depth 8: 511 nodes × 3 bytes), so the u16
    //length prefix cannot truncate.
    uint16_t len = static_cast<uint16_t>(bytes.size());
    fs.write(reinterpret_cast<const char*>(&len), sizeof(len));
    if (len > 0)
        fs.write(reinterpret_cast<const char*>(bytes.data()), len);
}

//Option B v1.3 fixed default-value record: tag + both 4-byte channels +
//the 8-byte channels (v1.13) + the always-present u16 expr length with
//its v2.3 payload. Shared by the formal-default block and the 0.9.10
//interface-constant block so the two records cannot drift (the second
//site lives in the interface record TU, see ModuleSaverRecords.h).
void WriteDefaultValueDesc(std::ostream& fs, const DefaultValueDesc& dv)
{
    fs.write(reinterpret_cast<const char*>(&dv.tag), sizeof(dv.tag));
    fs.write(reinterpret_cast<const char*>(&dv.intValue),
             sizeof(dv.intValue));
    fs.write(reinterpret_cast<const char*>(&dv.floatValue),
             sizeof(dv.floatValue));
    fs.write(reinterpret_cast<const char*>(&dv.stringIdx),
             sizeof(dv.stringIdx));
    fs.write(reinterpret_cast<const char*>(&dv.longValue),
             sizeof(dv.longValue));
    fs.write(reinterpret_cast<const char*>(&dv.doubleValue),
             sizeof(dv.doubleValue));
    uint16_t exprLen = static_cast<uint16_t>(dv.exprBytes.size());
    fs.write(reinterpret_cast<const char*>(&exprLen), sizeof(exprLen));
    if (exprLen > 0)
        fs.write(reinterpret_cast<const char*>(dv.exprBytes.data()),
                 exprLen);
}

bool WriteCompiledModule(std::ostream& fs, const CompiledModule& mod,
                         const std::string& entryKey) {
    if (!fs.good()) return false;
    //The four enum tables (names/keys + v2.9 method indices/access)
    //stay parallel by construction (RegisterEnums + RegisterEnumMethods,
    //Import's merge push, EnumSymbolSlot, ModuleLoader). The writer is
    //the single chokepoint to refuse a diverging module: the reader
    //consumes exactly enumCount keys and enumCount method rows, so a
    //mismatch would desync every later section into a misleading
    //"bad import count" failure.
    if (mod.enumKeys.size() != mod.enumNames.size()
        || mod.enumMethodIndices.size() != mod.enumNames.size()
        || mod.enumMethodAccess.size() != mod.enumNames.size())
        return false;

    // Magic
    const char(&magic)[8] = NCU_MAGIC;
    fs.write(magic, 8);

    // Version (shared with ModuleLoader via CompiledModule.h — single source)
    uint16_t majorVer = NCU_FORMAT_MAJOR, minorVer = NCU_FORMAT_MINOR;
    fs.write(reinterpret_cast<const char*>(&majorVer), sizeof(majorVer));
    fs.write(reinterpret_cast<const char*>(&minorVer), sizeof(minorVer));

    // v2.0: the module's dotted path (package identity of the unit).
    uint16_t modulePathLen =
        static_cast<uint16_t>(mod.modulePath.size());
    fs.write(reinterpret_cast<const char*>(&modulePathLen),
             sizeof(modulePathLen));
    fs.write(mod.modulePath.c_str(), modulePathLen);

    // Module name
    uint32_t nameLen = static_cast<uint32_t>(mod.name.size());
    fs.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
    fs.write(mod.name.c_str(), nameLen);

    // v2.0: the entry function's qualified key (empty = no entry). The
    // loader re-resolves the index by name — table-layout-dependent
    // indices are not serializable.
    uint16_t entryKeyLen = static_cast<uint16_t>(entryKey.size());
    fs.write(reinterpret_cast<const char*>(&entryKeyLen),
             sizeof(entryKeyLen));
    fs.write(entryKey.c_str(), entryKeyLen);

    // v2.0 phase 6: cross-unit import slots are appended after ALL own
    // sections (see the reader for the layout). Own entries occupy table
    // indices 0..n-1; slots n..n+m are placeholders resolved by nlink at
    // load time. entryPoint is NOT serialized — the entry is the .npkg
    // header's record, or <modulePath>.main for a bare unit.

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
        //0.9.10 v2.4: static-member flag — name-based instance dispatch
        //skips these records; the frame layout differs too (no __this).
        uint8_t staticFlag = func.isStatic ? 1 : 0;
        fs.write(reinterpret_cast<const char*>(&staticFlag),
                 sizeof(staticFlag));

        //Option B v1.3: per-formal default-value descriptors. Always
        //emitted (count first) so reader can skip even when no defaults.
        //Count == func.defaultValues.size(); formals without defaults
        //carry tag=RTK_Void to preserve positional alignment, so the
        //vector is naturally dense.
        uint16_t defaultCount = static_cast<uint16_t>(
            func.defaultValues.size());
        fs.write(reinterpret_cast<const char*>(&defaultCount),
                 sizeof(defaultCount));
        //The record layout lives in WriteDefaultValueDesc (shared with
        //the interface-constant block below).
        for (const auto& dv : func.defaultValues)
            WriteDefaultValueDesc(fs, dv);

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
        //v1.14: declPc (debugger scope visibility) between the kind byte
        //and the name length.
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
            fs.write(reinterpret_cast<const char*>(&ld.declPc),
                     sizeof(ld.declPc));
            uint32_t lnameLen = static_cast<uint32_t>(ld.name.size());
            fs.write(reinterpret_cast<const char*>(&lnameLen),
                     sizeof(lnameLen));
            fs.write(ld.name.c_str(), lnameLen);
        }

        //v1.9 (debugger): source file path, always emitted (len+bytes,
        //like the name field above). Readers gate on minorVer>=9.
        uint32_t sfileLen = static_cast<uint32_t>(func.sourceFile.size());
        fs.write(reinterpret_cast<const char*>(&sfileLen), sizeof(sfileLen));
        fs.write(func.sourceFile.c_str(), sfileLen);

        //v1.12: true formal type descriptors, count first (methods carry
        //paramCount-1: the implicit this slot has no descriptor, mirroring
        //defaultValues). Each entry is u8 flags | len-prefixed descriptor.
        uint16_t paramDescCount = static_cast<uint16_t>(
            func.paramTypeDescs.size());
        fs.write(reinterpret_cast<const char*>(&paramDescCount),
                 sizeof(paramDescCount));
        for (const auto& ptd : func.paramTypeDescs) {
            fs.write(reinterpret_cast<const char*>(&ptd.flags),
                     sizeof(ptd.flags));
            WriteLenPrefixedTypeDesc(fs, ptd.type);
        }

        //v1.12: return type descriptor — only when the function returns a
        //value (RTK_Void keeps ctor/void records shaped as before).
        if (func.returnTypeKind != RTK_Void)
            WriteLenPrefixedTypeDesc(fs, func.returnTypeDesc);

        //K2 slice 5: eval-area handle mask, the record's trailing field
        //(uint32 byte count + packed 2-bit classes; see EvalSlotClass).
        //Always emitted — bandless records carry a zero count.
        uint32_t maskBytes = static_cast<uint32_t>(
            func.evalAreaHandleMask.size());
        fs.write(reinterpret_cast<const char*>(&maskBytes),
                 sizeof(maskBytes));
        if (maskBytes > 0)
            fs.write(reinterpret_cast<const char*>(
                func.evalAreaHandleMask.data()), maskBytes);
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

        //v2.12 (v0.12.4): const-aggregate marker (see CompiledStruct).
        fs.write(reinterpret_cast<const char*>(&st.isConstAggregate),
                 sizeof(st.isConstAggregate));

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

        //v1.12: per-field type descriptors (len==0 = not expressible).
        for (size_t i = 0; i < st.fieldCount; ++i) {
            WriteLenPrefixedTypeDesc(fs,
                (i < st.fieldTypeDescs.size()) ? st.fieldTypeDescs[i]
                                               : TypeDesc{});
        }

        //v2.6: ctor indices in declaration order; the cross-module
        //ownerOrdinal indexes this list (overloads share the bare name).
        uint16_t ctorCount = static_cast<uint16_t>(
            st.constructorIndices.size());
        fs.write(reinterpret_cast<const char*>(&ctorCount), sizeof(ctorCount));
        for (auto idx : st.constructorIndices)
            fs.write(reinterpret_cast<const char*>(&idx), sizeof(idx));

        //v2.6: dense per-field defaults (RTK_Void = none). The count is
        //fieldCount by construction (stub structs emit RTK_Void blocks).
        uint16_t defCount = st.fieldCount;
        fs.write(reinterpret_cast<const char*>(&defCount), sizeof(defCount));
        for (size_t i = 0; i < st.fieldCount; ++i) {
            WriteDefaultValueDesc(fs,
                (i < st.fieldDefaults.size()) ? st.fieldDefaults[i]
                                              : DefaultValueDesc{});
        }

        //0.9.12 (v2.7): declared access per ctor, parallel to
        //constructorIndices (count rides on ctorCount) — the consumer
        //stub rebuilds each struct ctor with its visibility.
        for (auto a : st.constructorAccess)
            fs.write(reinterpret_cast<const char*>(&a), sizeof(a));
    }

    // Class descriptors (wire layout in ModuleSaverRecordsClasses.cpp —
    // one table per TU; the v2.13 static block landed with the split)
    WriteClassRecords(fs, mod);

    //0.9.10 v2.4 + 0.9.12 v2.7: interface statics / constants /
    //instance-method signature tables (the wire layout lives with the
    //writer in ModuleSaverRecordsInterfaces.cpp).
    WriteInterfaceRecords(fs, mod);

    //Array type descriptors
    uint32_t arrayTypeCount = static_cast<uint32_t>(mod.arrayTypes.size());
    fs.write(reinterpret_cast<const char*>(&arrayTypeCount),
             sizeof(arrayTypeCount));
    for (auto& at : mod.arrayTypes) {
        fs.write(reinterpret_cast<const char*>(&at.elemKind),
                 sizeof(at.elemKind));
        fs.write(reinterpret_cast<const char*>(&at.elemTypeIdx),
                 sizeof(at.elemTypeIdx));
        //Stage-2: rank rides the record (v2.10).
        fs.write(reinterpret_cast<const char*>(&at.rank),
                 sizeof(at.rank));
    }

    //Phase 8e-9b + v2.0 + v2.9: the enum record table (names, qualified
    //keys, member-method tables) — writer in
    //ModuleSaverRecordsEnums.cpp (one table per TU).
    WriteEnumRecords(fs, mod);

    //v2.13 (v0.12.6): the module's static slot table — one entry per
    //runtime-addressed static (mutable class statics, runtime-const
    //fields, static locals; compile-time-folded constants occupy no
    //slot). Per slot: name (u32 len + bytes), ownerKey (u32 len + bytes),
    //the ownerKind / isRuntimeConst / hasInitializer bytes, and a
    //length-prefixed TypeDesc. Class/interface records reference these
    //slots by index (their staticFields blocks were written earlier).
    {
        uint32_t slotCount = static_cast<uint32_t>(mod.staticSlots.size());
        fs.write(reinterpret_cast<const char*>(&slotCount),
                 sizeof(slotCount));
        for (const auto& slot : mod.staticSlots) {
            uint32_t slotNameLen = static_cast<uint32_t>(slot.name.size());
            fs.write(reinterpret_cast<const char*>(&slotNameLen),
                     sizeof(slotNameLen));
            fs.write(slot.name.c_str(), slotNameLen);
            uint32_t ownerKeyLen = static_cast<uint32_t>(slot.ownerKey.size());
            fs.write(reinterpret_cast<const char*>(&ownerKeyLen),
                     sizeof(ownerKeyLen));
            fs.write(slot.ownerKey.c_str(), ownerKeyLen);
            fs.write(reinterpret_cast<const char*>(&slot.ownerKind),
                     sizeof(slot.ownerKind));
            fs.write(reinterpret_cast<const char*>(&slot.isRuntimeConst),
                     sizeof(slot.isRuntimeConst));
            fs.write(reinterpret_cast<const char*>(&slot.hasInitializer),
                     sizeof(slot.hasInitializer));
            WriteLenPrefixedTypeDesc(fs, slot.type);
        }
    }

    //v2.0 phase 6: cross-unit import slots, one section per table, in
    //slot order (own entries occupy 0..n-1, imports n..n+m). Per entry:
    //u16 modulePathLen + bytes, u16 nameLen + bytes, u32 paramCount; the
    //function section additionally carries u16 ownerClassKeyLen + bytes
    //(empty = namespace-level function; see SymbolImport), the
    //0.9.10 v2.4 static flag byte (linkage identity: a static and an
    //instance member of the same name and arity are distinct slots), and
    //the 0.9.11 v2.6 ownerKind byte + ownerOrdinal u16 (owner table kind
    //and constructor ordinal — overloads share the bare name).
    auto writeImports = [&fs](const std::vector<CompiledModule::SymbolImport>&
                                  imports, bool withOwnerKey)
    {
        uint32_t count = static_cast<uint32_t>(imports.size());
        fs.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& imp : imports)
        {
            uint16_t modulePathLen =
                static_cast<uint16_t>(imp.modulePath.size());
            fs.write(reinterpret_cast<const char*>(&modulePathLen),
                     sizeof(modulePathLen));
            fs.write(imp.modulePath.c_str(), modulePathLen);
            uint16_t nameLen = static_cast<uint16_t>(imp.name.size());
            fs.write(reinterpret_cast<const char*>(&nameLen),
                     sizeof(nameLen));
            fs.write(imp.name.c_str(), nameLen);
            fs.write(reinterpret_cast<const char*>(&imp.paramCount),
                     sizeof(imp.paramCount));
            if (withOwnerKey)
            {
                uint16_t ownerLen =
                    static_cast<uint16_t>(imp.ownerClassKey.size());
                fs.write(reinterpret_cast<const char*>(&ownerLen),
                         sizeof(ownerLen));
                fs.write(imp.ownerClassKey.c_str(), ownerLen);
                uint8_t staticFlag = imp.isStatic ? 1 : 0;
                fs.write(reinterpret_cast<const char*>(&staticFlag),
                         sizeof(staticFlag));
                //v2.6: which table kind ownerClassKey addresses (0 =
                //namespace-level, 1 = class member, 2 = struct member)
                //plus the constructor ordinal within the owner's
                //constructorIndices (0xFFFF = not a constructor slot).
                fs.write(reinterpret_cast<const char*>(&imp.ownerKind),
                         sizeof(imp.ownerKind));
                fs.write(reinterpret_cast<const char*>(&imp.ownerOrdinal),
                         sizeof(imp.ownerOrdinal));
            }
        }
    };
    writeImports(mod.functionImports, true);
    writeImports(mod.classImports, false);
    writeImports(mod.structImports, false);
    writeImports(mod.enumImports, false);
    //v2.13: cross-module static-slot imports (cross-module static access,
    //resolved by nlink like the tables above). Same record shape as the
    //function section; ownerKind here follows the static-slot table's own
    //0 = class owner / 1 = interface owner domain (see CompiledModule).
    writeImports(mod.staticSlotImports, true);

    return fs.good();
}

} // namespace nlang
