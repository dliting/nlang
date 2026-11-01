/*---
    ModuleSaverRecordsClasses.cpp — .ncu class record table serialization
    (classes section), split from ModuleSaver.cpp (2026-11-02 source-size
    guard split, one table per TU, zero behavior change; the v2.13 static
    block joined the same round). The shared stream-level adapters
    WriteLenPrefixedTypeDesc / WriteDefaultValueDesc live in
    ModuleSaver.cpp via ModuleSaverRecords.h; the reader dual is
    ModuleLoaderRecordsClasses.cpp.
---*/
#include "ModuleSaverRecords.h"

namespace nlang {

//Class descriptors: u32 count, then per class the name (u32 len + bytes),
//fieldCount, superClassIdx, the v2.12 const-aggregate marker, the own-field
//block (names / type kinds / struct indices / class indices / type
//descriptors / access bytes), the member-function block (indices / access /
//virtuality), the constructor block (indices / access), and the v2.13
//static block below.
void WriteClassRecords(std::ostream& fs, const CompiledModule& mod)
{
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

        //v2.12 (v0.12.4): const-aggregate marker (see CompiledClass).
        fs.write(reinterpret_cast<const char*>(&cc.isConstAggregate),
                 sizeof(cc.isConstAggregate));

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

        //v1.12: per-field type descriptors (len==0 = not expressible).
        for (size_t i = 0; i < cc.fieldCount; ++i) {
            WriteLenPrefixedTypeDesc(fs,
                (i < cc.fieldTypeDescs.size()) ? cc.fieldTypeDescs[i]
                                               : TypeDesc{});
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

        //0.9.12 (v2.7): declared access and virtuality per method,
        //parallel to methodIndices (counts ride on methodCount — the
        //producer keeps the vectors parallel by construction, same
        //invariant as fieldAccess above). The consumer stub rebuilds
        //each member with its visibility, and the cross-module
        //override contract reads the virtuality.
        for (auto a : cc.methodAccess)
            fs.write(reinterpret_cast<const char*>(&a), sizeof(a));
        for (auto v : cc.methodVirtual)
            fs.write(reinterpret_cast<const char*>(&v), sizeof(v));

        //v2.6: ctor indices in declaration order; the cross-module
        //ownerOrdinal indexes this list (replaces v2.5's single index).
        uint16_t ctorCount = static_cast<uint16_t>(
            cc.constructorIndices.size());
        fs.write(reinterpret_cast<const char*>(&ctorCount), sizeof(ctorCount));
        for (auto idx : cc.constructorIndices)
            fs.write(reinterpret_cast<const char*>(&idx), sizeof(idx));

        //0.9.12 (v2.7): declared access per ctor, parallel to
        //constructorIndices (count rides on ctorCount). Replaces the
        //v2.4 publicStaticMethodIndices block, removed in place: every
        //member function — static included — now travels in
        //methodIndices, and staticness rides the function record's
        //isStatic flag.
        for (auto a : cc.constructorAccess)
            fs.write(reinterpret_cast<const char*>(&a), sizeof(a));

        //v2.13 (v0.12.6): declared static fields in textual order —
        //u16 count, then per field the name (u32 len + bytes), a
        //length-prefixed TypeDesc, the access byte, the isConst byte,
        //the u16 slot index, and a DefaultValueDesc record only when
        //slotIdx == 0xFFFF (the compile-time-folded arm carries its
        //value inline; slotted fields read their initializer from the
        //static-initializer function). The consumer stub rebuilds the
        //static surface from this block.
        uint16_t staticFieldCount = static_cast<uint16_t>(
            cc.staticFields.size());
        fs.write(reinterpret_cast<const char*>(&staticFieldCount),
                 sizeof(staticFieldCount));
        for (const auto& sf : cc.staticFields) {
            uint32_t sfNameLen = static_cast<uint32_t>(sf.name.size());
            fs.write(reinterpret_cast<const char*>(&sfNameLen),
                     sizeof(sfNameLen));
            fs.write(sf.name.c_str(), sfNameLen);
            WriteLenPrefixedTypeDesc(fs, sf.type);
            fs.write(reinterpret_cast<const char*>(&sf.access),
                     sizeof(sf.access));
            uint8_t constFlag = sf.isConst ? 1 : 0;
            fs.write(reinterpret_cast<const char*>(&constFlag),
                     sizeof(constFlag));
            fs.write(reinterpret_cast<const char*>(&sf.slotIdx),
                     sizeof(sf.slotIdx));
            if (sf.slotIdx == 0xFFFF)
                WriteDefaultValueDesc(fs, sf.foldedValue);
        }

        //v2.13 (v0.12.6): the class's lazy static-initializer function
        //(0xFFFF = none; u16 = function-index operand width).
        fs.write(reinterpret_cast<const char*>(&cc.staticInitFuncIdx),
                 sizeof(cc.staticInitFuncIdx));
    }
}

} // namespace nlang
