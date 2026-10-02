/*---
    NcuLoader.h — nloader：运行期闭包装载器（纯 I/O＋发现，零表变换）。
    设计：docs/dev/phase6_loader_design.md §4 步骤 1-2、§6。从入口产物
    （.ncu 文件或 .npkg 包）出发，按各单元符号导入槽的目标模块路径在
    包搜索路径上急切发现闭包：`<dir>/<路径>.ncu` 文件优先，其次搜索目录
    内 .npkg 成员（入口包自身成员表最优先——同包成员一起发布、一起解析）。
    装载校验（格式版本、成员校验和、模块身份）全部在任何用户字节码执行
    之前完成，全部问题一次报清。表合并与槽位解析归 nlink（NcuLinker）。
---*/
#pragma once
#include "nlang/vm/CompiledModule.h"
#include <string>
#include <vector>

namespace nlang
{

class NcuLoader
{
public:
	struct Options
	{
		//包搜索根，按优先级排序。调用方负责组装（nvm：CLI -I＞模块
		//目录＞NLANG_PATH＞exe 目录/stdlib 目录）。
		std::vector<std::string> searchDirs;
	};

	struct Result
	{
		//units[0] 是入口单元（NcuLinker 的闭包契约）；其余为发现顺序。
		std::vector<CompiledModule> units;
		//入口函数限定名（空＝无入口）。.npkg 入口记录优先于单元内
		//序列化的 entryKey（设计 §3：nproj 可指定入口单元）。
		std::string entryKey;
	};

	//装载失败（缺包/版本不符/校验和不符/身份错标/容器损坏）抛
	//std::runtime_error，全部诊断一次列出（设计 §6）。
	static Result LoadClosure(const std::string& entryArtifact,
		const Options& options);
};

} //namespace nlang
