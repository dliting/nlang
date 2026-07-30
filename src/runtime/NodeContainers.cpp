#include "NodeContainers.h"
#include <memory>

namespace nlang
{

ImmutableNodeList ImmutableNodeList::s_Null;


ImmutableNodeList::ImmutableNodeList() : m_upItems(new inner_list())
{

}

ImmutableNodeList::~ImmutableNodeList()
{
	for (auto pNode : *m_upItems)
		DeleteNode(pNode);
}

} //namespace nlang