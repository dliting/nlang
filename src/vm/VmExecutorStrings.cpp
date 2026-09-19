/*---
VmExecutorStrings.cpp — string object store: minting + accessors.

Separate TU by the same discipline as VmExecutorDebug.cpp: the executor
core only gains call sites; all store logic lives here. RTK_String slots
hold 1-based handles into m_stringObjs (0 = null, reads as ""). Task 1
is a pure representation migration: every object is immortal (sweep is
implemented in Task 2), and concatenation is still content concat (cons
arrives in Task 3 — AllocConsString / StrVal's flatten branch are
declared/complete but never fed Cons nodes yet).
---*/
#include "VmExecutor.h"
#include <utility>
#include <vector>

namespace nlang {

//Raw slot from the free list or the back of the store. Every mint funnels
//through here — the single place m_gcPending gets set for strings.
int32_t VmExecutor::AllocStringObj() {
    int32_t handle;
    if (!m_strFreeList.empty()) {
        handle = m_strFreeList.back();
        m_strFreeList.pop_back();
        m_stringObjs[static_cast<size_t>(handle)] = StrObj{};
    } else {
        handle = static_cast<int32_t>(m_stringObjs.size());
        m_stringObjs.emplace_back();
        m_strMarkBits.push_back(false);
    }
    m_gcPending = true;
    return handle;
}

bool VmExecutor::IsLiveStringHandle(int32_t handle) const {
    return handle > 0
        && static_cast<size_t>(handle) < m_stringObjs.size()
        && static_cast<uint8_t>(m_stringObjs[static_cast<size_t>(handle)].form)
               != kStrFormDead;
}

//Task 1: flat mint only (cons arrives in Task 3, interning in Task 4).
int32_t VmExecutor::MintNewString(const std::string& content) {
    int32_t handle = AllocStringObj();
    m_stringObjs[static_cast<size_t>(handle)].str = content;
    return handle;
}

int32_t VmExecutor::MintConstantString(const std::string& content) {
    int32_t handle = MintNewString(content);
    m_stringObjs[static_cast<size_t>(handle)].immortal = true;
    return handle;
}

//Execution-path accessor: guaranteed Flat on return. A Cons node is
//flattened iteratively (left-leaning `s += x` chains are arbitrarily
//deep — no recursion) and rewritten in place (V8 ThinString idea:
//every existing handle sees the flat content). Callers must not hold
//the returned reference across further string minting (vector growth
//invalidates it — same discipline as the old ReadStrArg by-value rule).
const std::string& VmExecutor::StrVal(int32_t handle) {
    if (!IsLiveStringHandle(handle))
        return m_stringObjs[static_cast<size_t>(m_emptyStrHandle)].str;
    StrObj& so = m_stringObjs[static_cast<size_t>(handle)];
    if (so.form != StrObj::Form::Cons)   //Flat (Cons exists from Task 3)
        return so.str;
    //Explicit right-then-left stack: pops in left-to-right order.
    std::string flat;
    std::vector<int32_t> work;
    work.push_back(handle);
    while (!work.empty()) {
        int32_t h = work.back();
        work.pop_back();
        if (h == 0) continue;   //null side contributes nothing
        const StrObj& n = m_stringObjs[static_cast<size_t>(h)];
        if (n.form == StrObj::Form::Cons) {
            work.push_back(n.right);
            work.push_back(n.left);
        } else {
            flat += n.str;
        }
    }
    so.str = std::move(flat);
    so.form = StrObj::Form::Flat;
    so.left = so.right = 0;
    return so.str;
}

//Frozen-view accessor: flattens into a local buffer without touching the
//node (debugger contract: const formatters MUST NOT touch the NLang heap).
std::string VmExecutor::StrValCopy(int32_t handle) const {
    if (!IsLiveStringHandle(handle))
        return std::string();
    const StrObj& so = m_stringObjs[static_cast<size_t>(handle)];
    if (so.form != StrObj::Form::Cons)
        return so.str;   //Flat fast path: no work vector, direct copy
    std::string flat;
    std::vector<int32_t> work;
    work.push_back(handle);
    while (!work.empty()) {
        int32_t h = work.back();
        work.pop_back();
        if (h == 0) continue;
        const StrObj& n = m_stringObjs[static_cast<size_t>(h)];
        if (n.form == StrObj::Form::Cons) {
            work.push_back(n.right);
            work.push_back(n.left);
        } else {
            flat += n.str;
        }
    }
    return flat;
}

} // namespace nlang
