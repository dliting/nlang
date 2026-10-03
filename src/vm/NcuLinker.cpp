/*---
    NcuLinker.cpp — nlink 链接器主体：对等合并→占位槽解析→统一重映射。
    内核同源于已删除的 backend/Import.cpp Phase A/B（编译期合并路径，
    d2 整链移除），但语义改为「N 个对等映像合并」（原为「并入既有
    目标」，设计 §5）。Pass B（占位槽解析）在 NcuLinkerSlots.cpp
    （2026-10-02 尺寸守卫触发的拆分）。
---*/
#include "NcuLinker.h"
#include "NcuLinkerRemap.h"
#include "NcuLinkerSlots.h"
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace nlang {
namespace {

//本单元实际压入合并表的条目下标（去重命中的不记——它们的元数据由
//首个提供方单元负责重映射，二次重映射会改错数据）。
struct UnitPushes
{
	std::vector<uint32_t> classes;
	std::vector<uint32_t> structs;
	std::vector<uint32_t> functions;
};

//(name, paramCount, intrinsicId) → 合并下标。只有纯内建桩（无字节码、
//行为＝intrinsic 派发）允许去重；用户函数永不按名去重——裸方法键
//（"add"/"toString"）跨类同名同参合法并存。
using BuiltinStubIndex =
	std::map<std::tuple<std::string, uint32_t, uint16_t>, uint32_t>;

static std::string JoinProblems(const std::vector<std::string>& problems) {
    std::string joined;
    for (size_t i = 0; i < problems.size(); ++i) {
        if (i) joined += "\n  ";
        joined += problems[i];
    }
    return joined;
}

//全量表内查找；查不到即映像损坏（映射按构造覆盖单元全部下标）。
static uint32_t Lookup(const std::unordered_map<uint32_t, uint32_t>& m,
                       uint32_t idx) {
    auto it = m.find(idx);
    if (it == m.end())
        throw std::runtime_error("nlink: table index "
            + std::to_string(idx) + " has no mapping (corrupt image)");
    return it->second;
}

//闭包形状与槽位不变量：单元身份唯一、各导入节不大于对应表、
//enumKeys/enumNames 平行。任何违反都是损坏或误用，立即拒绝。
static void ValidateClosure(const std::vector<CompiledModule>& units) {
    if (units.empty())
        throw std::runtime_error("nlink: empty link closure");
    std::unordered_set<std::string> paths;
    for (const auto& u : units) {
        if (u.modulePath.empty())
            throw std::runtime_error(
                "nlink: a unit has no module path (unlinked or corrupt image)");
        if (!paths.insert(u.modulePath).second)
            throw std::runtime_error("nlink: duplicate module path '"
                + u.modulePath + "' in the link closure");
        if (u.enumKeys.size() != u.enumNames.size())
            throw std::runtime_error("nlink: unit '" + u.modulePath
                + "': enumKeys and enumNames diverge (corrupt image)");
        auto checkTable = [&](const char* kind, size_t imports,
                              size_t table) {
            if (imports > table)
                throw std::runtime_error("nlink: unit '" + u.modulePath
                    + "': " + kind + "Imports (" + std::to_string(imports)
                    + ") exceed the " + kind + " table ("
                    + std::to_string(table) + ")");
        };
        checkTable("function", u.functionImports.size(), u.functions.size());
        checkTable("class", u.classImports.size(), u.classes.size());
        checkTable("struct", u.structImports.size(), u.structs.size());
        checkTable("enum", u.enumImports.size(), u.enumNames.size());
    }
}

// --- Pass A：各单元自有条目并入合并表（占位槽永不拷贝） ---

static void MergeStrings(const CompiledModule& u, CompiledModule& merged,
                         NcuOperandMaps& m,
                         std::unordered_map<std::string, uint32_t>& interned) {
    for (uint32_t i = 0; i < u.stringConstants.size(); ++i) {
        auto it = interned.find(u.stringConstants[i]);
        if (it != interned.end()) {
            m.strings[i] = it->second;
            continue;
        }
        m.strings[i] = static_cast<uint32_t>(merged.stringConstants.size());
        merged.stringConstants.push_back(u.stringConstants[i]);
        interned.emplace(u.stringConstants[i], m.strings[i]);
    }
}

//同限定名 == 同类型（两侧都是表键；跨单元重名只有各单元都注册的无主
//内建类，同版本 ncc 产出恒等）。
static void MergeClasses(const CompiledModule& u, CompiledModule& merged,
                         NcuOperandMaps& m, UnitPushes& p) {
    const size_t ownCount = u.classes.size() - u.classImports.size();
    for (uint32_t i = 0; i < ownCount; ++i) {
        int existing = merged.FindClass(u.classes[i].name);
        if (existing >= 0) {
            m.classes[i] = static_cast<uint32_t>(existing);
            continue;
        }
        m.classes[i] = static_cast<uint32_t>(merged.classes.size());
        merged.classes.push_back(u.classes[i]);
        p.classes.push_back(m.classes[i]);
    }
}

static void MergeStructs(const CompiledModule& u, CompiledModule& merged,
                         NcuOperandMaps& m, UnitPushes& p) {
    const size_t ownCount = u.structs.size() - u.structImports.size();
    for (uint32_t i = 0; i < ownCount; ++i) {
        int existing = merged.FindStruct(u.structs[i].name);
        if (existing >= 0) {
            m.structs[i] = static_cast<uint32_t>(existing);
            continue;
        }
        m.structs[i] = static_cast<uint32_t>(merged.structs.size());
        merged.structs.push_back(u.structs[i]);
        p.structs.push_back(m.structs[i]);
    }
}

//enumNames 不携带类型身份，去重一律按 enumKeys 的限定键；占位槽
//（空 names＋目标键）不拷贝，由槽解析直接定址。
static void MergeEnums(const CompiledModule& u, CompiledModule& merged,
                       NcuOperandMaps& m) {
    const size_t ownCount = u.enumKeys.size() - u.enumImports.size();
    for (uint32_t i = 0; i < ownCount; ++i) {
        int existing = merged.FindEnum(u.enumKeys[i]);
        if (existing >= 0) {
            m.enums[i] = static_cast<uint32_t>(existing);
            continue;
        }
        m.enums[i] = static_cast<uint32_t>(merged.enumKeys.size());
        merged.enumNames.push_back(u.enumNames[i]);
        merged.enumKeys.push_back(u.enumKeys[i]);
    }
}

static void MergeFunctions(const CompiledModule& u, CompiledModule& merged,
                           NcuOperandMaps& m, UnitPushes& p,
                           BuiltinStubIndex& builtinStubs) {
    const size_t ownCount = u.functions.size() - u.functionImports.size();
    for (uint32_t i = 0; i < ownCount; ++i) {
        const CompiledFunction& src = u.functions[i];
        //纯内建桩＝intrinsicId 非空且无字节码；带字节码的记录是真实
        //函数体，即使撞上（名，形参数，intrinsicId）也永不并入去重桶。
        const bool pureIntrinsicStub =
            src.intrinsicId != INTR_None && src.bytecode.empty();
        if (pureIntrinsicStub) {
            const auto key = std::make_tuple(src.name,
                static_cast<uint32_t>(src.paramCount), src.intrinsicId);
            auto it = builtinStubs.find(key);
            if (it != builtinStubs.end()) {
                m.functions[i] = it->second;
                continue;
            }
            m.functions[i] = static_cast<uint32_t>(merged.functions.size());
            merged.functions.push_back(src);
            builtinStubs.emplace(key, m.functions[i]);
        } else {
            m.functions[i] = static_cast<uint32_t>(merged.functions.size());
            merged.functions.push_back(src);
        }
        p.functions.push_back(m.functions[i]);
    }
}

static void MergeUnitTables(const std::vector<CompiledModule>& units,
                            CompiledModule& merged,
                            std::vector<NcuOperandMaps>& maps,
                            std::vector<UnitPushes>& pushes) {
    std::unordered_map<std::string, uint32_t> internedStrings;
    BuiltinStubIndex builtinStubs;
    for (size_t ui = 0; ui < units.size(); ++ui) {
        MergeStrings(units[ui], merged, maps[ui], internedStrings);
        MergeClasses(units[ui], merged, maps[ui], pushes[ui]);
        MergeStructs(units[ui], merged, maps[ui], pushes[ui]);
        MergeEnums(units[ui], merged, maps[ui]);
        MergeFunctions(units[ui], merged, maps[ui], pushes[ui],
                       builtinStubs);
    }
}

// --- Pass C：元数据与字节码统一重映射（槽解析后映射才完整） ---

static void MergeArrayTypes(const CompiledModule& u, CompiledModule& merged,
                            NcuOperandMaps& m) {
    //elemTypeIdx 可能是占位槽下标（元素类型来自他单元），须先经
    //struct/class 映射再按（元素 kind，元素下标）去重。
    for (uint32_t i = 0; i < u.arrayTypes.size(); ++i) {
        CompiledArrayType at = u.arrayTypes[i];
        if (at.elemTypeIdx != 0xFFFF) {
            if (at.elemKind == RTK_Struct)
                at.elemTypeIdx = static_cast<uint16_t>(
                    Lookup(m.structs, at.elemTypeIdx));
            else if (at.elemKind == RTK_Class)
                at.elemTypeIdx = static_cast<uint16_t>(
                    Lookup(m.classes, at.elemTypeIdx));
        }
        int existing = merged.FindArray(at.elemKind, at.elemTypeIdx);
        if (existing >= 0) {
            m.arrayTypes[i] = static_cast<uint32_t>(existing);
            continue;
        }
        m.arrayTypes[i] = static_cast<uint32_t>(merged.arrayTypes.size());
        merged.arrayTypes.push_back(at);
    }
}

static void RemapClassMetadata(CompiledModule& merged,
                               const NcuOperandMaps& m,
                               const std::vector<uint32_t>& pushed) {
    for (uint32_t idx : pushed) {
        CompiledClass& cc = merged.classes[idx];
        if (cc.superClassIdx >= 0)
            cc.superClassIdx = static_cast<int16_t>(
                Lookup(m.classes, static_cast<uint32_t>(cc.superClassIdx)));
        for (auto& i : cc.fieldStructIndices)
            if (i != 0xFFFF) i = static_cast<uint16_t>(Lookup(m.structs, i));
        for (auto& i : cc.fieldClassIndices)
            if (i != 0xFFFF) i = static_cast<uint16_t>(Lookup(m.classes, i));
        for (auto& i : cc.methodIndices)
            if (i != 0xFFFF) i = static_cast<uint16_t>(Lookup(m.functions, i));
        if (cc.constructorIdx != 0xFFFF)
            cc.constructorIdx = static_cast<uint16_t>(
                Lookup(m.functions, cc.constructorIdx));
        for (auto& td : cc.fieldTypeDescs)
            RemapTypeDesc(td, m.structs, m.classes);
    }
}

static void RemapStructMetadata(CompiledModule& merged,
                                const NcuOperandMaps& m,
                                const std::vector<uint32_t>& pushed) {
    for (uint32_t idx : pushed) {
        CompiledStruct& cs = merged.structs[idx];
        for (auto& i : cs.fieldStructIndices)
            if (i != 0xFFFF) i = static_cast<uint16_t>(Lookup(m.structs, i));
        for (auto& i : cs.fieldClassIndices)
            if (i != 0xFFFF) i = static_cast<uint16_t>(Lookup(m.classes, i));
        for (auto& td : cs.fieldTypeDescs)
            RemapTypeDesc(td, m.structs, m.classes);
    }
}

//合并记录已整体拷贝（含单元局部编码的 bytecode/描述符/默认值），这里
//以本单元映射原地重映射。
static void FinalizeFunction(CompiledFunction& f, const NcuOperandMaps& m) {
    NcuRemapBytecodeOperands(f.bytecode, m,
        [&f](const char* tableKind, uint16_t idx) {
            throw std::runtime_error(std::string("nlink: function '")
                + f.name + "': bytecode operand references " + tableKind
                + " index " + std::to_string(idx)
                + " outside the unit's table (corrupt image)");
        });
    for (auto& tb : f.tryBlocks)
        if (tb.exceptionClassIdx != 0xFFFF)
            tb.exceptionClassIdx = static_cast<uint16_t>(
                Lookup(m.classes, tb.exceptionClassIdx));
    for (auto& ptd : f.paramTypeDescs)
        RemapTypeDesc(ptd.type, m.structs, m.classes);
    RemapTypeDesc(f.returnTypeDesc, m.structs, m.classes);
    //defaultValues.stringIdx 是生产者侧字符串下标（CompiledModule.h 的
    //DefaultValueDesc 契约），跨单元合并必须换算。
    for (auto& dv : f.defaultValues)
        if (dv.tag == RTK_String)
            dv.stringIdx = Lookup(m.strings, dv.stringIdx);
}

static void FinalizeUnits(const std::vector<CompiledModule>& units,
                          CompiledModule& merged,
                          std::vector<NcuOperandMaps>& maps,
                          const std::vector<UnitPushes>& pushes) {
    for (size_t ui = 0; ui < units.size(); ++ui) {
        MergeArrayTypes(units[ui], merged, maps[ui]);
        RemapClassMetadata(merged, maps[ui], pushes[ui].classes);
        RemapStructMetadata(merged, maps[ui], pushes[ui].structs);
        for (uint32_t idx : pushes[ui].functions)
            FinalizeFunction(merged.functions[idx], maps[ui]);
    }
}

static void ResolveEntry(CompiledModule& merged, const std::string& entryKey) {
    if (entryKey.empty()) {
        merged.entryPoint = -1;
        return;
    }
    int idx = merged.FindFunction(entryKey);
    if (idx < 0)
        throw std::runtime_error("nlink: entry key '" + entryKey
            + "' names no function in the linked module");
    merged.entryPoint = idx;
}

} //namespace

CompiledModule NcuLinker::Link(std::vector<CompiledModule> units,
                               const std::string& entryKey) {
    ValidateClosure(units);
    CompiledModule merged;
    merged.modulePath = units[0].modulePath;
    merged.name = units[0].name;
    std::vector<NcuOperandMaps> maps(units.size());
    std::vector<UnitPushes> pushes(units.size());
    MergeUnitTables(units, merged, maps, pushes);
    std::vector<std::string> problems;
    NcuResolveSlots(units, merged, maps, problems);
    if (!problems.empty())
        throw std::runtime_error("nlink failed:\n  "
            + JoinProblems(problems));
    FinalizeUnits(units, merged, maps, pushes);
    ResolveEntry(merged, entryKey);
    return merged;
}

} //namespace nlang
