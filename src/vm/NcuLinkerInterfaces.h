/*---
    NcuLinkerInterfaces.h — nlink Pass A的接口表合并（内部件）。
    v0.12.6 Task 8从NcuLinker.cpp拆出（尺寸守卫）：UnitPushes是Pass A
    合并与Pass C收尾共享的压表记录，随本头文件对两个翻译单元可见。
---*/
#pragma once
#include "NcuLinkerRemap.h"
#include "nlang/vm/CompiledModule.h"
#include <cstdint>
#include <vector>

namespace nlang
{

//本单元实际压入合并表的条目下标（去重命中的不记——它们的元数据由
//首个提供方单元负责重映射，二次重映射会改错数据）。
struct UnitPushes
{
	std::vector<uint32_t> classes;
	std::vector<uint32_t> structs;
	std::vector<uint32_t> functions;
	std::vector<uint32_t> interfaces;
	std::vector<uint32_t> enums;
	std::vector<uint32_t> slots;
};

//按限定键去重合并接口表。v0.12.6 Task 8：消费方单元为导入接口桩压入
//的无内容占位行就地让位于生产方的富行（NcuLinkerInterfaces.cpp）。
void MergeInterfaces(const CompiledModule& u, CompiledModule& merged,
	NcuOperandMaps& m, std::vector<UnitPushes>& pushes, size_t ui);

} //namespace nlang
