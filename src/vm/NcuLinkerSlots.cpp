/*---
    NcuLinkerSlots.cpp — nlink Pass B：占位槽解析（NcuLinkerSlots.h
    的实现）。函数槽两分支（命名空间级/属主键）＋类型槽按键定址；
    诊断一次报清。从 NcuLinker.cpp 拆出（2026-10-02 尺寸守卫触发的
    拆分，零行为变化）。
---*/
#include "NcuLinkerSlots.h"
#include <unordered_map>

namespace nlang {
namespace {

//一条函数导入在目标单元内的解析结果。hit＝目标单元局部函数下标；
//hits＝候选数（>1 即歧义）。
struct FunctionSlotHit {
    int hit = -1;
    size_t hits = 0;
    bool ownerClassFound = false;   //owner-keyed: class reached, member missed
};

//空属主键分支：命名空间级函数，限定键在目标单元自有函数区定址；
//多重同名同参命中即歧义（不猜）。
static FunctionSlotHit ResolveNamespacedFunction(
    const CompiledModule& tgt, const CompiledModule::SymbolImport& imp) {
    FunctionSlotHit r;
    const size_t tgtOwn = tgt.functions.size() - tgt.functionImports.size();
    for (uint32_t k = 0; k < tgtOwn; ++k) {
        //0.9.10 v2.4: staticness is part of the linkage identity — an
        //interface static and a same-shape free function are distinct.
        if (tgt.functions[k].name == imp.name
            && tgt.functions[k].paramCount == imp.paramCount
            && tgt.functions[k].isStatic == imp.isStatic) {
            if (r.hit < 0) r.hit = static_cast<int>(k);
            ++r.hits;
        }
    }
    return r;
}

//属主类别措辞（诊断共用）：ownerKind 1=class、2=struct，其余按类型
//泛称（属主键记录只可能是前两者）。
static const char* OwnerKindWord(const CompiledModule::SymbolImport& imp) {
    return imp.ownerKind == 2 ? "struct" : "class";
}

//按属主限定键在目标单元自有表区定位属主类型（ownerKind 分派 class/
//struct——裸键本身无法区分 struct 构造器与同名 class 方法）。命中返回
//true 并填 tc/ts（恰其一非空）。
static bool FindOwnerType(const CompiledModule& tgt,
    const CompiledModule::SymbolImport& imp,
    const CompiledClass*& tc, const CompiledStruct*& ts) {
    if (imp.ownerKind == 2) {
        const size_t tgtStOwn = tgt.structs.size() - tgt.structImports.size();
        for (size_t k = 0; k < tgtStOwn; ++k) {
            if (tgt.structs[k].name == imp.ownerClassKey) {
                ts = &tgt.structs[k];
                return true;
            }
        }
        return false;
    }
    const size_t tgtClsOwn = tgt.classes.size() - tgt.classImports.size();
    for (size_t k = 0; k < tgtClsOwn; ++k) {
        if (tgt.classes[k].name == imp.ownerClassKey) {
            tc = &tgt.classes[k];
            return true;
        }
    }
    return false;
}

//属主键分支（方法/构造器）：先按限定键定位属主类型（FindOwnerType），
//再类型内消歧。构造器按 ctorOrdinal 定址属主的 constructorIndices
//（v2.6：重载构造器同名同参，名＋形参数无法单选），方法按裸名＋形参
//数＋静态性（类内无重载；struct 无方法，非构造器属主键记录落空）。
//表键保持裸名，属主键只活在槽记录里。
static FunctionSlotHit ResolveOwnerKeyedFunction(
    const CompiledModule& tgt, const CompiledModule::SymbolImport& imp) {
    FunctionSlotHit r;
    const size_t tgtOwn = tgt.functions.size() - tgt.functionImports.size();
    //构造器记录：ctorOrdinal 是权威选择器（0xFFFF＝非构造器槽）。
    const bool isCtorSlot = imp.ctorOrdinal != 0xFFFF;
    const CompiledClass* tc = nullptr;
    const CompiledStruct* ts = nullptr;
    if (!FindOwnerType(tgt, imp, tc, ts))
        return r;
    r.ownerClassFound = true;
    if (isCtorSlot) {
        const std::vector<uint16_t>& ctors = tc
            ? tc->constructorIndices : ts->constructorIndices;
        //越界＝目标类型没有该序号的构造器（签名未命中，非闭包缺
        //单元）；形参数再校验一次，防序号与签名漂移后错连。
        if (imp.ctorOrdinal < ctors.size()) {
            const uint16_t ci = ctors[imp.ctorOrdinal];
            if (ci != 0xFFFF && ci < tgtOwn
                && tgt.functions[ci].paramCount == imp.paramCount) {
                r.hit = static_cast<int>(ci);
                r.hits = 1;
            }
        }
        return r;
    }
    //方法记录：struct 无方法，直接落空（诊断走"无该签名成员"措辞）。
    if (!tc)
        return r;
    for (uint16_t mi : tc->methodIndices) {
        //0.9.10 v2.4: 静态性参与消歧（同名静态/实例方法对跨线缆后仍
        //是两个槽）。
        if (mi < tgtOwn && tgt.functions[mi].name == imp.name
            && tgt.functions[mi].paramCount == imp.paramCount
            && tgt.functions[mi].isStatic == imp.isStatic) {
            if (r.hit < 0) r.hit = static_cast<int>(mi);
            ++r.hits;
        }
    }
    return r;
}

//每条函数槽诊断共享的导入描述：函数名、形参数、属主类（属主键记录）
//与来源模块。
static std::string DescribeFunctionImport(
    const CompiledModule::SymbolImport& imp) {
    //0.9.10 v2.4: staticness participates in slot identity — name it.
    return "function '" + imp.name
        + "' (paramCount " + std::to_string(imp.paramCount) + ")"
        + (imp.isStatic ? " static" : "")
        + (imp.ownerClassKey.empty()
               ? std::string()
               : std::string(" of ") + OwnerKindWord(imp)
                     + " '" + imp.ownerClassKey + "'")
        + " from module '" + imp.modulePath + "'";
}

//未命中措辞：属主键记录且属主类已找到＝签名未命中（形参数不符或缺
//席），不得归咎闭包缺单元；其余（含命名空间级）照旧。
static std::string FunctionMissLine(const CompiledModule& u,
                                    const std::string& what,
                                    const CompiledModule::SymbolImport& imp,
                                    bool ownerClassFound) {
    if (!imp.ownerClassKey.empty() && ownerClassFound)
        return "unit '" + u.modulePath + "': import of " + what
            + " fails: " + OwnerKindWord(imp) + " '" + imp.ownerClassKey
            + "' provides no member with that signature";
    return "unit '" + u.modulePath + "': imported " + what
        + " is not provided by any unit in the closure";
}

//函数槽解析（§4.4 两分支）：按属主键分派到上面两个命中函数，结果统一
//落图；全部未解析项一次收集。
static void ResolveFunctionSlots(const std::vector<CompiledModule>& units,
    const std::unordered_map<std::string, size_t>& unitOf,
    std::vector<NcuOperandMaps>& maps,
    std::vector<std::string>& problems) {
    for (size_t ui = 0; ui < units.size(); ++ui) {
        const CompiledModule& u = units[ui];
        const size_t ownCount = u.functions.size()
                              - u.functionImports.size();
        for (size_t j = 0; j < u.functionImports.size(); ++j) {
            const auto& imp = u.functionImports[j];
            const std::string what = DescribeFunctionImport(imp);
            auto owner = unitOf.find(imp.modulePath);
            if (owner == unitOf.end()) {
                problems.push_back("unit '" + u.modulePath + "': import of "
                    + what + " fails: module '" + imp.modulePath
                    + "' is not in the link closure");
                continue;
            }
            const CompiledModule& tgt = units[owner->second];
            const FunctionSlotHit r = imp.ownerClassKey.empty()
                ? ResolveNamespacedFunction(tgt, imp)
                : ResolveOwnerKeyedFunction(tgt, imp);
            const uint32_t slot = static_cast<uint32_t>(ownCount + j);
            if (r.hits == 0)
                problems.push_back(FunctionMissLine(u, what, imp,
                                                    r.ownerClassFound));
            else if (r.hits > 1)
                problems.push_back("unit '" + u.modulePath
                    + "': imported " + what + " is ambiguous: "
                    + std::to_string(r.hits) + " matching records in "
                    + (imp.ownerClassKey.empty()
                           ? std::string("the owning unit")
                           : std::string(OwnerKindWord(imp))
                                 + " '" + imp.ownerClassKey + "'"));
            else
                maps[ui].functions[slot] =
                    maps[owner->second]
                        .functions[static_cast<uint32_t>(r.hit)];
        }
    }
}

static std::string UnresolvedTypeLine(const CompiledModule& u,
                                      const char* kind,
                                      const CompiledModule::SymbolImport& imp) {
    return "unit '" + u.modulePath + "': imported " + kind + " '"
        + imp.name + "' from module '" + imp.modulePath
        + "' is not provided by any unit in the closure";
}

//类型槽按限定键在合并表定址（键内嵌包前缀，闭包内全局唯一；无主
//内建类各单元恒等，Pass A 已去重）。
static void ResolveTypeSlots(const std::vector<CompiledModule>& units,
                             const CompiledModule& merged,
                             std::vector<NcuOperandMaps>& maps,
                             std::vector<std::string>& problems) {
    for (size_t ui = 0; ui < units.size(); ++ui) {
        const CompiledModule& u = units[ui];
        NcuOperandMaps& m = maps[ui];
        const size_t clsOwn = u.classes.size() - u.classImports.size();
        for (size_t j = 0; j < u.classImports.size(); ++j) {
            const auto& imp = u.classImports[j];
            int idx = merged.FindClass(imp.name);
            if (idx < 0)
                problems.push_back(UnresolvedTypeLine(u, "class", imp));
            else
                m.classes[static_cast<uint32_t>(clsOwn + j)] =
                    static_cast<uint32_t>(idx);
        }
        const size_t stOwn = u.structs.size() - u.structImports.size();
        for (size_t j = 0; j < u.structImports.size(); ++j) {
            const auto& imp = u.structImports[j];
            int idx = merged.FindStruct(imp.name);
            if (idx < 0)
                problems.push_back(UnresolvedTypeLine(u, "struct", imp));
            else
                m.structs[static_cast<uint32_t>(stOwn + j)] =
                    static_cast<uint32_t>(idx);
        }
        const size_t enOwn = u.enumNames.size() - u.enumImports.size();
        for (size_t j = 0; j < u.enumImports.size(); ++j) {
            const auto& imp = u.enumImports[j];
            int idx = merged.FindEnum(imp.name);
            if (idx < 0)
                problems.push_back(UnresolvedTypeLine(u, "enum", imp));
            else
                m.enums[static_cast<uint32_t>(enOwn + j)] =
                    static_cast<uint32_t>(idx);
        }
    }
}

} //namespace

void NcuResolveSlots(const std::vector<CompiledModule>& units,
                     const CompiledModule& merged,
                     std::vector<NcuOperandMaps>& maps,
                     std::vector<std::string>& problems) {
    //闭包身份表：modulePath → units 下标（闭包内唯一，ValidateClosure
    //已保证）。
    std::unordered_map<std::string, size_t> unitOf;
    for (size_t i = 0; i < units.size(); ++i)
        unitOf.emplace(units[i].modulePath, i);
    ResolveFunctionSlots(units, unitOf, maps, problems);
    ResolveTypeSlots(units, merged, maps, problems);
}

} //namespace nlang
