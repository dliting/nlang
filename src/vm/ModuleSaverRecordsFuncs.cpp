/*---
ModuleSaverRecordsFuncs.cpp — 函数表写入（从ModuleSaver.cpp一表一TU抽出，0.13.10）。
---*/
#include "nlang/core/CompiledModule.h"
#include "ModuleSaverRecords.h"

namespace nlang {

//v1.12起逐版累积的函数记录（v2.23：成对实例化描述符索引descIdx）。
void WriteFunctionRecords(std::ostream& fs, const CompiledModule& mod)
{
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
        //v2.16: instance-method flag (the frame's slot-0 __this) — the
        //ABI v12 native bridge reads it to locate the receiver handle.
        uint8_t instanceMethodFlag = func.isInstanceMethod ? 1 : 0;
        fs.write(reinterpret_cast<const char*>(&instanceMethodFlag),
                 sizeof(instanceMethodFlag));
        //v2.18: abstract declaration — signature-only record carrying
        //the obligation; dispatch must find an override.
        uint8_t abstractFlag = func.isAbstract ? 1 : 0;
        fs.write(reinterpret_cast<const char*>(&abstractFlag),
                 sizeof(abstractFlag));
        //v2.17: generic-function declaration — the flag plus the declared
        //type-parameter names (the class rows carry the same pair). The
        //loader rebuilds isGeneric/typeParamNames from this; the wire has
        //no other source for FREE functions, whose genericity names no
        //owning class row.
        uint8_t genericFlag = func.isGeneric ? 1 : 0;
        fs.write(reinterpret_cast<const char*>(&genericFlag),
                 sizeof(genericFlag));
        //v2.23: lambda record — the function was written as a lambda
        //expression; toString rendering reads the flag instead of
        //guessing from the synthetic name shape.
        uint8_t lambdaFlag = func.isLambda ? 1 : 0;
        fs.write(reinterpret_cast<const char*>(&lambdaFlag),
                 sizeof(lambdaFlag));
        uint16_t fnTypeParamCount = static_cast<uint16_t>(
            func.typeParamNames.size());
        fs.write(reinterpret_cast<const char*>(&fnTypeParamCount),
                 sizeof(fnTypeParamCount));
        for (const auto& tpName : func.typeParamNames) {
            uint32_t tpLen = static_cast<uint32_t>(tpName.size());
            fs.write(reinterpret_cast<const char*>(&tpLen), sizeof(tpLen));
            fs.write(tpName.c_str(), tpLen);
        }

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

        //K2 slice 5: eval-area handle mask (uint32 byte count + packed
        //2-bit classes; see EvalSlotClass). Always emitted — bandless
        //records carry a zero count.
        uint32_t maskBytes = static_cast<uint32_t>(
            func.evalAreaHandleMask.size());
        fs.write(reinterpret_cast<const char*>(&maskBytes),
                 sizeof(maskBytes));
        if (maskBytes > 0)
            fs.write(reinterpret_cast<const char*>(
                func.evalAreaHandleMask.data()), maskBytes);

        //v2.20 (0.13.8 item 5): the declared access byte, the record's
        //trailing field — a free function's only access carrier on the wire.
        fs.write(reinterpret_cast<const char*>(&func.access),
                 sizeof(func.access));
    }
}

} // namespace nlang
