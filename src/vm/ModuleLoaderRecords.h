/*---
    ModuleLoaderRecords.h — .ncu 复合记录表解析器（读取侧内部实现）。
    ModuleLoader.cpp 拆分（源尺寸守卫触发的可维护性重构，零行为变化）：
    LoadFromBytes 的骨架与标量段留在 ModuleLoader.cpp，三张复合记录表
    （functions / structs / classes）的逐字段解析搬到这里。仅 vm 模块内
    部使用，不安装到 include/。
---*/
#pragma once
#include "nlang/vm/CompiledModule.h"
#include <istream>

namespace nlang {

//Parse the functions section (funcCount entries) from the current stream
//position. Throws std::runtime_error on truncation or a bad count/length.
void ReadFunctionRecords(std::istream& fs, CompiledModule& mod);

//Parse the struct descriptors section. Same failure contract.
void ReadStructRecords(std::istream& fs, CompiledModule& mod);

//Parse the class descriptors section. Same failure contract.
void ReadClassRecords(std::istream& fs, CompiledModule& mod);

} // namespace nlang
