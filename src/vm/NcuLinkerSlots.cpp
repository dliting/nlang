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
        if (tgt.functions[k].name == imp.name
            && tgt.functions[k].paramCount == imp.paramCount) {
            if (r.hit < 0) r.hit = static_cast<int>(k);
            ++r.hits;
        }
    }
    return r;
}

//属主键分支（方法/构造器）：先按限定键在目标单元自有类区找属主类，
//再类内消歧——名＝裸类名走 constructorIdx，否则 methodIndices 按裸名
//（形参数为校验；类内无重载）。表键保持裸名，属主键只活在槽记录里。
static FunctionSlotHit ResolveOwnerKeyedFunction(
    const CompiledModule& tgt, const CompiledModule::SymbolImport& imp) {
    FunctionSlotHit r;
    const size_t tgtOwn = tgt.functions.size() - tgt.functionImports.size();
    const size_t tgtClsOwn = tgt.classes.size() - tgt.classImports.size();
    int clsIdx = -1;
    for (size_t k = 0; k < tgtClsOwn; ++k) {
        if (tgt.classes[k].name == imp.ownerClassKey) {
            clsIdx = static_cast<int>(k);
            break;
        }
    }
    if (clsIdx < 0)
        return r;
    r.ownerClassFound = true;
    const CompiledClass& tc = tgt.classes[clsIdx];
    const size_t dot = imp.ownerClassKey.rfind('.');
    const std::string bareClassName = imp.ownerClassKey.substr(
        dot == std::string::npos ? 0 : dot + 1);
    if (imp.name == bareClassName) {
        //构造器：类自身的构造下标（形参数为校验）。
        const uint16_t ci = tc.constructorIdx;
        if (ci != 0xFFFF && ci < tgtOwn
            && tgt.functions[ci].paramCount == imp.paramCount) {
            r.hit = static_cast<int>(ci);
            r.hits = 1;
        }
        return r;
    }
    for (uint16_t mi : tc.methodIndices) {
        if (mi < tgtOwn && tgt.functions[mi].name == imp.name
            && tgt.functions[mi].paramCount == imp.paramCount) {
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
    return "function '" + imp.name
        + "' (paramCount " + std::to_string(imp.paramCount) + ")"
        + (imp.ownerClassKey.empty()
               ? std::string()
               : " of class '" + imp.ownerClassKey + "'")
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
            + " fails: class '" + imp.ownerClassKey
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
                           : "class '" + imp.ownerClassKey + "'"));
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
