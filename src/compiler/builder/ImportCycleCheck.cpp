/*---
    ImportCycleCheck.cpp — D13编译期import环检测（ImportCycleCheck.h）。
    三色DFS：gray命中即环，路径从栈中该节点首次出现处截取（直接式报
    全环a -> b -> a，既有式报环段b -> c -> b——前缀的无辜路径不入诊断）。
    std::map承载边表保证节点序确定，同一图跨运行诊断稳定。
---*/
#include "ImportCycleCheck.h"
#include <algorithm>

namespace nlang
{

namespace
{
enum Mark { kWhite, kGray, kBlack };

//The three-color DFS carried as a struct: the visit recursion reads as
//methods instead of a nested std::function lambda (size-guard split,
//zero behavior change).
class CycleFinder
{
public:
	CycleFinder(const std::string &rootModule,
		const std::vector<std::string> &rootDirectImports,
		const std::map<std::string,
			std::vector<std::string>> &imageImports)
		: m_rootModule(rootModule),
		  m_rootDirectImports(rootDirectImports),
		  m_imageImports(imageImports)
	{
	}

	//Cycle path from the root; empty = acyclic.
	std::string Find()
	{
		Visit(m_rootModule);
		return std::move(m_found);
	}

private:
	//Adjacency: the root's own direct imports outrank the map (a root
	//compiled unit may also appear there as another root's edge table;
	//its own list is the authoritative one). Nodes absent from both
	//carry no out-edges — leaf units.
	const std::vector<std::string> *EdgesOf(
		const std::string &node) const
	{
		if (node == m_rootModule)
			return &m_rootDirectImports;
		const auto it = m_imageImports.find(node);
		return it == m_imageImports.end() ? nullptr : &it->second;
	}

	void Visit(const std::string &node)
	{
		if (!m_found.empty())
			return;
		m_marks[node] = kGray;
		m_stack.push_back(node);
		if (const auto *outs = EdgesOf(node))
		{
			for (const auto &next : *outs)
			{
				const auto it = m_marks.find(next);
				if (it == m_marks.end())
				{
					Visit(next);
					if (!m_found.empty())
						return;
				}
				else if (it->second == kGray)
				{
					ReconstructCycle(next);
					return;
				}
			}
		}
		m_stack.pop_back();
		m_marks[node] = kBlack;
	}

	//Reconstruct from the target's first stack occurrence — the
	//segment inside the current DFS path that closes the cycle.
	void ReconstructCycle(const std::string &target)
	{
		const auto at = std::find(m_stack.begin(), m_stack.end(),
			target);
		for (auto k = at; k != m_stack.end(); ++k)
		{
			if (!m_found.empty())
				m_found += " -> ";
			m_found += *k;
		}
		m_found += " -> " + target;
	}

	const std::string &m_rootModule;
	const std::vector<std::string> &m_rootDirectImports;
	const std::map<std::string, std::vector<std::string>>
		&m_imageImports;
	std::map<std::string, Mark> m_marks;
	std::vector<std::string> m_stack;
	std::string m_found;   //cycle path; empty = acyclic
};
} //namespace

std::string ImportCycleCheck::Check(const std::string& rootModule,
	const std::vector<std::string>& rootDirectImports,
	const std::map<std::string, std::vector<std::string>>& imageImports)
{
	return CycleFinder(rootModule, rootDirectImports,
		imageImports).Find();
}

} //namespace nlang
