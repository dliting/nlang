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

//v1.12: one length-prefixed type descriptor (u16 len + wire bytes);
//len==0 = "no descriptor" — the reader keeps the NonSerialized default.
//Shared by the param/return/per-field sites below and the interface TU.
void WriteLenPrefixedTypeDesc(std::ostream& fs, const TypeDesc& td)
{
    std::vector<uint8_t> bytes;
    AppendTypeDescBytes(bytes, td);
    //The depth cap bounds a descriptor well under 64 KiB (worst case: a
    //full binary Dict tree at depth 8, 511 nodes × 3 bytes) — the u16
    //length prefix cannot truncate.
    uint16_t len = static_cast<uint16_t>(bytes.size());
    fs.write(reinterpret_cast<const char*>(&len), sizeof(len));
    if (len > 0)
        fs.write(reinterpret_cast<const char*>(bytes.data()), len);
}

//Option B v1.3 fixed default-value record: tag + both 4-byte channels +
//the 8-byte channels (v1.13) + the always-present u16 expr length with
//its v2.3 payload. Shared by the formal-default and 0.9.10 interface-
//constant blocks so the records cannot drift (ModuleSaverRecords.h).
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
    //The five enum tables (names/keys + v2.9 method indices/access +
    //v2.20 type access) stay parallel by construction (RegisterEnums +
    //RegisterEnumMethods, Import's merge push, EnumSymbolSlot,
    //ModuleLoader). The writer is the single chokepoint to refuse a
    //diverging module: the reader consumes exactly enumCount keys, enumCount
    //method rows and enumCount access bytes, so a mismatch would desync
    //every later section into a misleading "bad import count" failure.
    if (mod.enumKeys.size() != mod.enumNames.size()
        || mod.enumMethodIndices.size() != mod.enumNames.size()
        || mod.enumMethodAccess.size() != mod.enumNames.size()
        || mod.enumAccess.size() != mod.enumNames.size())
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

    // v2.15 (v0.13.1): the generic-instantiation descriptor table — one
    // row per registered descriptor (see DescriptorEntry). Per entry:
    // u16 spellingLen + bytes, u16 compositionLen + bytes (empty composition =
    // static spelling row). The u16 caps match the intern spelling's
    // practical bound (canonical names stay well under 64 KiB).
    uint32_t descCount =
        static_cast<uint32_t>(mod.descriptorTable.size());
    fs.write(reinterpret_cast<const char*>(&descCount), sizeof(descCount));
    for (const auto& desc : mod.descriptorTable) {
        uint16_t spellingLen =
            static_cast<uint16_t>(desc.spelling.size());
        fs.write(reinterpret_cast<const char*>(&spellingLen),
                 sizeof(spellingLen));
        fs.write(desc.spelling.c_str(), spellingLen);
        uint16_t compositionLen =
            static_cast<uint16_t>(desc.composition.size());
        fs.write(reinterpret_cast<const char*>(&compositionLen),
                 sizeof(compositionLen));
        if (compositionLen > 0)
            fs.write(reinterpret_cast<const char*>(desc.composition.data()),
                     compositionLen);
    }

    // Functions (wire layout in ModuleSaverRecordsFuncs.cpp —
    // one table per TU)
    WriteFunctionRecords(fs, mod);

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
        //ownerOrdinal indexes this list (overloads share the unqualified name).
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

        //v2.20 (0.13.8 item 5): the struct's declared access, the
        //record's trailing byte.
        fs.write(reinterpret_cast<const char*>(&st.access),
                 sizeof(st.access));
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
    //and constructor ordinal — overloads share the unqualified name).
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
