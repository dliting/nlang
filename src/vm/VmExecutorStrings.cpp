/*---
VmExecutorStrings.cpp — string object store: minting + accessors.

Separate TU by the same discipline as VmExecutorDebug.cpp: the executor
core only gains call sites; all store logic lives here. RTK_String slots
hold 1-based handles into m_stringObjs (0 = null, reads as ""). Since
Task 3, concatenation allocates a zero-copy Cons node; StrVal flattens
in place on first read, StrValCopy serves frozen views, and MarkString
traces cons children so a dropped chain is reclaimed whole.
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

//Content-aware mint (the only interning site class): short strings consult
//the weak table and reuse the live handle on a hit — strings are
//immutable, so sharing is aliasing-safe. Cons allocation and in-place
//flattening never come through here, which is what keeps the uniqueness
//invariant (at most one live interned object per content) intact: a
//second mint of live content always hits the table, and a hit on a dead
//entry falls through and re-registers.
int32_t VmExecutor::MintNewString(std::string content) {
    if (content.size() <= kShortStringMaxBytes) {
        auto it = m_shortStrTable.find(content);
        if (it != m_shortStrTable.end() && IsLiveStringHandle(it->second))
            return it->second;
    }
    int32_t handle = AllocStringObj();
    StrObj& so = m_stringObjs[static_cast<size_t>(handle)];
    if (content.size() <= kShortStringMaxBytes) {
        so.interned = true;
        m_shortStrTable[content] = handle;   //map keys its own copy
    }
    so.str = std::move(content);
    return handle;
}

//Constants take the same intern path — a short constant shares its object
//with runtime mints of the same content — and additionally never die
//(their table entries are therefore never purified either).
int32_t VmExecutor::MintConstantString(const std::string& content) {
    int32_t handle = MintNewString(content);
    m_stringObjs[static_cast<size_t>(handle)].immortal = true;
    return handle;
}

//Equality fast-path predicate (OP_Eq_str/OP_Ne_str): only a LIVE handle
//with the interned bit qualifies — handle identity is then equivalent to
//content equality by the uniqueness invariant plus immutability.
bool VmExecutor::IsInternedString(int32_t handle) const {
    return IsLiveStringHandle(handle)
        && m_stringObjs[static_cast<size_t>(handle)].interned;
}

//O(1) zero-copy concatenation: content-blind, so never interned (intern
//sites are content-aware mints only). Null sides stay 0 and contribute
//nothing at flatten time. Callers must not pass the same handle on both
//sides — that would build a DAG and the flatten walk is a tree walk
//(OP_Concat_str guards this by materializing the operand first).
int32_t VmExecutor::AllocConsString(int32_t left, int32_t right) {
    int32_t handle = AllocStringObj();
    StrObj& so = m_stringObjs[static_cast<size_t>(handle)];
    so.form = StrObj::Form::Cons;
    so.left = left;
    so.right = right;
    return handle;
}

//Shared flatten walk for both accessors: collects a cons subtree's
//content left-to-right into out. Explicit right-then-left stack (pops in
//left-to-right order) — left-leaning `s += x` chains are arbitrarily
//deep, so no recursion.
void VmExecutor::FlattenInto(std::string& out, int32_t handle) const {
    std::vector<int32_t> work;
    work.push_back(handle);
    while (!work.empty()) {
        int32_t h = work.back();
        work.pop_back();
        if (h == 0 || !IsLiveStringHandle(h))
            continue;   //null side contributes nothing; dead reads as ""
        const StrObj& n = m_stringObjs[static_cast<size_t>(h)];
        if (n.form == StrObj::Form::Cons) {
            work.push_back(n.right);
            work.push_back(n.left);
        } else {
            out += n.str;
        }
    }
}

//Execution-path accessor: guaranteed Flat on return. A Cons node is
//flattened (via the shared walk) and rewritten in place (V8 ThinString
//idea: every existing handle sees the flat content). Callers must not
//hold the returned reference across further string minting (vector
//growth invalidates it — same discipline as the old ReadStrArg by-value
//rule).
const std::string& VmExecutor::StrVal(int32_t handle) {
    if (!IsLiveStringHandle(handle))
        return m_stringObjs[static_cast<size_t>(m_emptyStrHandle)].str;
    StrObj& so = m_stringObjs[static_cast<size_t>(handle)];
    if (so.form != StrObj::Form::Cons)
        return so.str;
    std::string flat;
    FlattenInto(flat, handle);
    so.str = std::move(flat);
    so.form = StrObj::Form::Flat;
    so.left = so.right = 0;
    return so.str;
}

//Frozen-view accessor: flattens into a local buffer without touching the
//node (debugger contract: const formatters MUST NOT touch the NLang heap).
std::string VmExecutor::StrValCopy(int32_t handle) const {
    std::string flat;
    if (IsLiveStringHandle(handle))
        FlattenInto(flat, handle);
    return flat;
}

//GC mark face for one string handle: a live cons keeps its whole subtree
//alive. Iterative worklist (left-leaning chains are arbitrarily deep —
//no recursion). Handle validation is local (root faces pass raw handles;
//0 = null and out-of-range are simply not marked).
void VmExecutor::MarkString(int32_t handle) {
    std::vector<int32_t> work;
    auto mark = [&](int32_t h) {
        if (!IsLiveStringHandle(h) || m_strMarkBits[static_cast<size_t>(h)])
            return;
        m_strMarkBits[static_cast<size_t>(h)] = true;
        work.push_back(h);
    };
    mark(handle);
    while (!work.empty()) {
        int32_t h = work.back();
        work.pop_back();
        const StrObj& so = m_stringObjs[static_cast<size_t>(h)];
        if (so.form == StrObj::Form::Cons) {
            mark(so.left);
            mark(so.right);
        }
    }
}

//Independent of the struct-heap sweep but inside the same CollectGarbage
//(mark completed for both stores first). Immortal objects (constants)
//never die; dead slots get the form sentinel and join the free list.
void VmExecutor::SweepStrings() {
    m_strFreeList.clear();
    for (size_t i = 1; i < m_stringObjs.size(); ++i) {
        StrObj& so = m_stringObjs[i];
        if (static_cast<uint8_t>(so.form) == kStrFormDead) continue;
        if (so.immortal || m_strMarkBits[i]) continue;
        //Weak-intern purification: drop the dying object's table entry so
        //a later mint of the same content allocates fresh instead of
        //resurrecting a dead (or slot-reused) handle.
        if (so.interned)
            m_shortStrTable.erase(so.str);
        so = StrObj{};   //release the buffer
        so.form = static_cast<StrObj::Form>(kStrFormDead);
        m_strFreeList.push_back(static_cast<int32_t>(i));
    }
    //Pin the no-stale-bits invariant at both ends: MarkPhase's head assign
    //already guarantees a clean vector, and AllocStringObj's freelist
    //branch never sets mark bits — clearing here makes the invariant hold
    //after every collection, not only before every mark.
    m_strMarkBits.assign(m_stringObjs.size(), false);
}

size_t VmExecutor::LiveStringObjectCount() const {
    size_t liveCount = 0;
    for (const StrObj& so : m_stringObjs)
        if (static_cast<uint8_t>(so.form) != kStrFormDead) ++liveCount;
    return liveCount;
}

} // namespace nlang
