/*---
    ImportCycleCheck.h — D13编译期import环检测（v0.12.6）。
    输入是编译器已持有的边图：根单元的直接import＋每张已装载
    .ncu镜像的SymbolImport模块路径。检出环返回"a -> b -> a"式
    路径，无环返回空串。
---*/
#pragma once

#include <map>
#include <string>
#include <vector>

namespace nlang
{

class ImportCycleCheck
{
public:
    //imageEdges keys are loaded image module paths; each value lists
    //the modules that image's import records reference. Nodes absent
    //from the map carry no out-edges (leaf units).
    static std::string Check(const std::string& rootModule,
        const std::vector<std::string>& rootDirectImports,
        const std::map<std::string, std::vector<std::string>>& imageImports);
};

} //namespace nlang
