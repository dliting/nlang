/*---
SymbolSlots.hpp — 跨单元引用的槽位解析（Phase 6 Task 3 核心）。

逐单元产码模型：每个单元的表只含自己的声明；字节码操作数是单元局部
下标。本单元的声明 → 既有查找；其它单元的声明 → 占位记录（最小：仅限
定名）＋导入槽（CompiledModule::SymbolImport），nlink 在加载期按限定名
解析槽位并把操作数重映射为全局下标。

两个不变量：
1. 同一单元内同一目标只占一个槽（按目标去重＝限定名＋所属模块路径，
   函数另加形参数——裸方法键跨包同名同参合法，缺模块路径会误并槽）；
2. 自有条目永远占据 0..n-1，占位槽从 n 开始、与导入节按序对应；
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

//目标的模块路径（OwnerOfContext → ModulePathOf）。仅跨单元引用需要。
//Context（祖先链）而非扁平属主：类/enum 成员自身无标记，扁平查找会把
//方法的属主单元降级成 NO_OWNER→空路径，链接器随即报「module '' is
//not in the link closure」。
inline std::string SymbolSlotModulePath(const ModuleRegistry &reg,
	const SnField &decl)
{
	return reg.ModulePathOf(reg.OwnerOfContext(decl));
}

//函数槽：按 modulePath+属主类键+名+形参数+静态性+属主类别+构造器序号
//去重后追加占位记录，返回局部槽位。ownerClassKey 空＝命名空间级函数
//（name 为限定键）；非空＝该类的方法或构造器（name 为裸名，构造器＝裸
//类名）——裸方法键跨类同名同参合法并存，属主键是槽记录里唯一的消歧来源
//（表键保持裸名，见设计 §2）。0.9.10 D5：静态性参与去重与导入记录——
//同名同参的静态/实例成员对是两个不同的槽，静态记录的形参数不含 this
//槽位。0.9.11（v2.6）：ownerKind 区分属主表类别（0 命名空间/1 class/
//2 struct，裸键本身无法区分 struct 构造器与同名 class 方法）；
//ownerOrdinal 是成员在属主表内的序号——构造器＝constructorIndices
//下标（重载构造器同名同参，如 Vec(int) 与 Vec(float)，仅靠名字+形参
//数无法区分），方法/静态方法＝methodIndices 剔除构造器条目后的位置
//（0.9.12 D-new-8：同名同参重载同理需要序号单选）；0xFFFF＝无序号。
inline uint32_t FunctionSymbolSlot(CompiledModule &mod,
	const std::string &modulePath, const std::string &name,
	uint32_t paramCount, const std::string &ownerClassKey = "",
	bool isStatic = false, uint8_t ownerKind = 0,
	uint16_t ownerOrdinal = 0xFFFF)
{
	const uint32_t ownCount = static_cast<uint32_t>(
		mod.functions.size() - mod.functionImports.size());
	for (uint32_t i = ownCount; i < mod.functions.size(); ++i)
	{
		if (mod.functions[i].name == name
			&& mod.functions[i].paramCount == paramCount
			&& mod.functions[i].isStatic == isStatic
			&& mod.functionImports[i - ownCount].modulePath == modulePath
			&& mod.functionImports[i - ownCount].ownerClassKey == ownerClassKey
			&& mod.functionImports[i - ownCount].ownerKind == ownerKind
			&& mod.functionImports[i - ownCount].ownerOrdinal == ownerOrdinal)
			return i;
	}
	CompiledFunction placeholder;
	placeholder.name = name;
	placeholder.paramCount = paramCount;
	placeholder.isStatic = isStatic;
	const uint32_t slot = static_cast<uint32_t>(mod.functions.size());
	mod.functions.push_back(std::move(placeholder));
	mod.functionImports.push_back(
		{modulePath, name, paramCount, ownerClassKey, isStatic,
		 ownerKind, ownerOrdinal});
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
