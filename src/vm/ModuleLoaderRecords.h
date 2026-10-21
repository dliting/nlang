/*---
    ModuleLoaderRecords.h — .ncu 复合记录表解析器（读取侧内部实现）。
    ModuleLoader.cpp 拆分（源尺寸守卫触发的可维护性重构，零行为变化）：
    LoadFromBytes 的骨架与标量段留在 ModuleLoader.cpp，复合记录表的逐字段
    解析按一表一 TU 分文件——functions / structs 在
    ModuleLoaderRecords.cpp，interfaces 在 ModuleLoaderRecordsInterfaces.cpp
    （0.9.12 v2.7 拆分），classes 在 ModuleLoaderRecordsClasses.cpp
    （2026-10-22 并发线合并后拆分）。仅 vm 模块内部使用，不安装到 include/。
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

//0.9.10 v2.4: parse the interface statics section. Same failure contract.
void ReadInterfaceRecords(std::istream& fs, CompiledModule& mod);

//Stream-level adapters shared by the record TUs (defined in
//ModuleLoaderRecords.cpp): one length-prefixed type descriptor and one
//fixed default-value record. Same failure contract.
TypeDesc ReadTypeDesc(std::istream& fs);
DefaultValueDesc ReadDefaultValueDesc(std::istream& fs);

} // namespace nlang
