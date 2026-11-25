/*---
    CanonicalArgSpelling.cpp — TypeArgSpelling.h契约的AST无关半：
    规范拼写与类型参数可组合性判定（层界拆分自TypeArgSpelling.cpp，
    拆分裁定见docs/dev/library_layering_design.md §6）。
---*/
#include <nlang/core/TypeArgSpelling.h>
//RTK_Array (the array arm of the canonical render).
#include <nlang/runtime/RuntimeTypeCodes.h>
#include <string>
#include <vector>

namespace nlang
{

std::string CanonicalArgSpelling(const TypeArgDesc& arg)
{
	if (arg.nested.empty())
		return arg.spelling;
	//Array arguments carry a complete leaf spelling ("int[]", "int[,]");
	//the nested element descriptor is structure for the dispatch paths,
	//never part of the canonical render.
	if (arg.resolvedKind == RTK_Array)
		return arg.spelling;
	std::string spelling = arg.spelling;
	spelling += "<";
	for (size_t i = 0; i < arg.nested.size(); ++i)
	{
		if (i)
			spelling += ",";
		spelling += CanonicalArgSpelling(arg.nested[i]);
	}
	spelling += ">";
	return spelling;
}

bool HasTypeParamArg(const std::vector<TypeArgDesc>& args)
{
	for (const auto& arg : args)
	{
		if (IsTypeParamArg(arg))
			return true;
		//A nested instantiation containing a type parameter anywhere in
		//its subtree is composable too.
		if (!arg.nested.empty() && HasTypeParamArg(arg.nested))
			return true;
	}
	return false;
}

} //namespace nlang
