/*---
    NcuLinker.h — nlink：加载期链接器（纯内存变换，零 I/O）。
    设计：docs/dev/phase6_loader_design.md §3/§4。N 份逐单元 .ncu 映像按
    限定名对等合并为运行期唯一 CompiledModule；占位槽按（目标模块路径，
    限定名，形参数）解析，全部操作数统一重映射为全局下标（原地改写、
    宽度不变——无新操作码，执行器/ndisasm 零改动）。未解析项一次报清。
---*/
#pragma once
#include "nlang/vm/CompiledModule.h"
#include <string>
#include <vector>

namespace nlang
{

class NcuLinker
{
public:
	//units[0] 是入口单元：其 modulePath/name 成为运行期模块身份。
	//entryKey 为入口函数限定名（空串＝无入口），在合并表上按名解析
	//（与 ModuleLoader 的 entryKey 契约一致）。闭包无法链接时抛
	//std::runtime_error，全部诊断一次列出（设计 §6）。
	static CompiledModule Link(std::vector<CompiledModule> units,
		const std::string& entryKey);
};

} //namespace nlang
