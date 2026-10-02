/*---
SymbolSlots.hpp — 跨单元引用的槽位解析（Phase 6 Task 3 核心）。

逐单元产码模型：每个单元的表只含自己的声明；字节码操作数是单元局部
下标。本单元的声明 → 既有查找；其它单元的声明 → 占位记录（最小：仅限
定名）＋导入槽（CompiledModule::SymbolImport），nlink 在加载期按限定名
解析槽位并把操作数重映射为全局下标。

三个不变量：
1. 同一单元内同一目标只占一个槽（按限定名去重，函数另加形参数）；
2. 自有条目永远占据 0..n-1，占位槽从 n 开始、与导入节按序对应；
3. 合并模式（VmBackend::MERGED_MODE，单文件模式）不产生占位槽——
   own 判定与查表走 VmBackend::IsOwnUnit 的既有路径。
---*/
#pragma once
#include "ModuleRegistry.h"
#include "nlang/compiler/SnMisc.h"
#include "nlang/vm/CompiledModule.h"
#include <cstdint>
#include <string>
#include <utility>

namespace nlang
{

//目标的模块路径（OwnerOf → ModulePathOf）。仅跨单元引用需要。
inline std::string SymbolSlotModulePath(const ModuleRegistry &reg,
	const SnField &decl)
{
	return reg.ModulePathOf(reg.OwnerOf(decl));
}

//函数槽：按 modulePath+限定名+形参数去重后追加占位记录，返回局部槽位。
inline uint32_t FunctionSymbolSlot(CompiledModule &mod,
	const std::string &modulePath, const std::string &name,
	uint32_t paramCount)
{
	const uint32_t ownCount = static_cast<uint32_t>(
		mod.functions.size() - mod.functionImports.size());
	for (uint32_t i = ownCount; i < mod.functions.size(); ++i)
	{
		if (mod.functions[i].name == name
			&& mod.functions[i].paramCount == paramCount)
			return i;
	}
	CompiledFunction placeholder;
	placeholder.name = name;
	placeholder.paramCount = paramCount;
	const uint32_t slot = static_cast<uint32_t>(mod.functions.size());
	mod.functions.push_back(std::move(placeholder));
	mod.functionImports.push_back({modulePath, name, paramCount});
	return slot;
}

//类型槽（class/struct/enum 表同构）：按限定名去重后追加占位记录。
//class/struct 的既有查找同时覆盖 own 条目与更早的占位槽。
inline uint32_t ClassSymbolSlot(CompiledModule &mod,
	const std::string &modulePath, const std::string &name)
{
	const int existing = mod.FindClass(name);
	if (existing >= 0)
		return static_cast<uint32_t>(existing);
	CompiledClass placeholder;
	placeholder.name = name;
	const uint32_t slot = static_cast<uint32_t>(mod.classes.size());
	mod.classes.push_back(std::move(placeholder));
	mod.classImports.push_back({modulePath, name, 0});
	return slot;
}

inline uint32_t StructSymbolSlot(CompiledModule &mod,
	const std::string &modulePath, const std::string &name)
{
	const int existing = mod.FindStruct(name);
	if (existing >= 0)
		return static_cast<uint32_t>(existing);
	CompiledStruct placeholder;
	placeholder.name = name;
	const uint32_t slot = static_cast<uint32_t>(mod.structs.size());
	mod.structs.push_back(std::move(placeholder));
	mod.structImports.push_back({modulePath, name, 0});
	return slot;
}

//enum 表：占位槽带目标键（enumNames={}＋enumKeys=键），去重按键——
//FindEnum 同时覆盖 own 条目与更早的占位槽（与 class/struct 同构）。
inline uint32_t EnumSymbolSlot(CompiledModule &mod,
	const std::string &modulePath, const std::string &name)
{
	const int existing = mod.FindEnum(name);
	if (existing >= 0)
		return static_cast<uint32_t>(existing);
	mod.enumNames.push_back({});
	mod.enumKeys.push_back(name);
	const uint32_t slot = static_cast<uint32_t>(mod.enumNames.size() - 1);
	mod.enumImports.push_back({modulePath, name, 0});
	return slot;
}

} // namespace nlang
