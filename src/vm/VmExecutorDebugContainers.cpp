/*---
    VmExecutorDebugContainers.cpp — debugger display of the container
    stores (List/Dict). Split from VmExecutorDebug.cpp when the file
    crossed the source-size guard; both TUs share the element cap and
    the heap-element renderer declared on VmExecutor.
---*/
#include "VmExecutor.h"

#include <string>
#include <utility>
#include <vector>

namespace nlang {

std::string VmExecutor::FormatDebugList(int32_t handle) const {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_listStore.size())
        return "List[0]{}";
    std::vector<int32_t> elems = SnapshotListElements(handle);
    std::string result = "List[" + std::to_string(elems.size()) + "]{";
    size_t bound = elems.size() < static_cast<size_t>(kMaxElementsShown)
        ? elems.size() : static_cast<size_t>(kMaxElementsShown);
    for (size_t i = 0; i < bound; ++i) {
        if (i > 0) result += ", ";
        result += FormatDebugElementHeap(elems[i]);
    }
    if (elems.size() > bound) result += ", ...";
    result += "}";
    return result;
}

std::string VmExecutor::FormatDebugDict(int32_t handle) const {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_dictStore.size())
        return "Dict[0]{}";
    std::vector<std::pair<int32_t, int32_t>> entries
        = SnapshotDictEntries(handle);
    std::string result = "Dict[" + std::to_string(entries.size()) + "]{";
    size_t bound = entries.size() < static_cast<size_t>(kMaxElementsShown)
        ? entries.size() : static_cast<size_t>(kMaxElementsShown);
    for (size_t i = 0; i < bound; ++i) {
        if (i > 0) result += ", ";
        result += FormatDebugElementHeap(entries[i].first) + ": "
            + FormatDebugElementHeap(entries[i].second);
    }
    if (entries.size() > bound) result += ", ...";
    result += "}";
    return result;
}

} //namespace nlang
