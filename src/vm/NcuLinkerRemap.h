/*---
    NcuLinkerRemap.h — nlink 的字节码操作数重映射游走（内部件）。
    指令步长表＋表索引操作数原地改写（宽度不变）。编译期合并路径
    （backend/Import.cpp 的 RemapBytecode）已在 d2 整链删除，本文件是
    该内核的唯一存续副本（设计 §5「Import.cpp 的归宿」）。
---*/
#pragma once
#include "BytecodeOps.h"
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace nlang
{

//一个单元的操作数重映射表：单元局部表下标（自有条目＋已解析的占位槽）
//→ 合并表全局下标。按构造全量覆盖，游走中查不到即映像损坏。
struct NcuOperandMaps
{
	std::unordered_map<uint32_t, uint32_t> strings;
	std::unordered_map<uint32_t, uint32_t> functions;
	std::unordered_map<uint32_t, uint32_t> classes;
	std::unordered_map<uint32_t, uint32_t> structs;
	std::unordered_map<uint32_t, uint32_t> arrayTypes;
	std::unordered_map<uint32_t, uint32_t> enums;
};

//原地改写全部表索引操作数（11 个重映射相关操作码族）。onUnmapped(
//tableKind, index) 在操作数查不到映射时触发。操作数偏移以
//BytecodeOps.h 的逐操作数注释为准。
void NcuRemapBytecodeOperands(std::vector<uint8_t>& bytecode,
	const NcuOperandMaps& maps,
	const std::function<void(const char* tableKind, uint16_t index)>&
		onUnmapped);

} //namespace nlang
