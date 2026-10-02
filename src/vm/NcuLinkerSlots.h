/*---
    NcuLinkerSlots.h — nlink Pass B：占位槽解析（内部件）。
    四张导入槽表按（目标模块路径，名，形参数，属主类键）在闭包内
    定址，结果写进各单元的操作数映射；全部未解析项一次收集。
---*/
#pragma once
#include "NcuLinkerRemap.h"
#include "nlang/vm/CompiledModule.h"
#include <string>
#include <vector>

namespace nlang
{

//解析 units 内每个单元的占位槽（function/class/struct/enum 四表），
//映射写入 maps；任何未解析/歧义项追加进 problems（不抛出，由调用方
//统一汇成链接诊断）。merged 是 Pass A 的对等合并表——类型槽在合并表
//上按限定键定址（键内嵌包前缀，闭包内全局唯一）。
void NcuResolveSlots(const std::vector<CompiledModule>& units,
	const CompiledModule& merged,
	std::vector<NcuOperandMaps>& maps,
	std::vector<std::string>& problems);

} //namespace nlang
