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
#include <algorithm>
#include <utility>
#include <vector>

namespace nlang {

//Raw slot from the calling carrier's string shard; growth is the rare
//slow path (one block appended under the Growth domain lock). Every
//mint funnels through here — the single place the GC pressure flag gets
//set for strings (K7: NoteGcPressure, the release-store into the
//atomic).
int32_t VmExecutor::AllocStringObj() {
    const int32_t handle = TakeStringSlot();
    m_stringObjs[static_cast<size_t>(handle)] = StrObj{};
    NoteGcPressure();
    return handle;
}

//K8: lock-free pop of the calling thread's own shard; on exhaustion take
//the Growth domain lock (double-checked) and append one block. Mirrors
//TakeHeapSlot's choreography.
int32_t VmExecutor::TakeStringSlot() {
    const size_t shard = CurrentShard();
    if (m_strShards.Empty(shard)) {
        DomainGuard g(m_domainLocks, DomainId::Growth);
        if (m_strShards.Empty(shard))   //double-check under the lock
            GrowStrShardBlockLocked(shard);
    }
    return m_strShards.Pop(shard);
}

//Caller must hold the Growth domain lock. Fresh slots stay default-Flat;
//the sweep recycles any unpopped ones into the shards (same net state:
//free). m_strMarkBits grows here to keep the parallel-array invariant
//(all three sync sites: this growth arm, MarkPhase's head, SweepStrings'
//tail).
void VmExecutor::GrowStrShardBlockLocked(size_t shard) {
    const int32_t base = static_cast<int32_t>(m_stringObjs.size());
    for (size_t k = 0; k < m_allocQuantum; ++k) {
        m_stringObjs.Emplace();
        m_strMarkBits.push_back(false);
    }
    m_strShards.PushBlockDescending(shard, base, m_allocQuantum);
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

//Single source of string equality (OP_Eq_str / OP_Ne_str): both operands
//interned compares handles (uniqueness invariant + immutability make
//identity and content the same relation); any other pair flattens and
//compares content. Two StrVal calls, no minting between them — the
//returned references stay valid (no store growth).
bool VmExecutor::StringsEqual(int32_t hA, int32_t hB) {
    //K7: the interned-flag read and the flatten below touch the
    //growing string store; the comparison itself also runs inside
    //this guard (StrVal returns references, which stay valid only
    //while the store cannot grow).
    GilEnsureGuard g(*this);
    if (IsInternedString(hA) && IsInternedString(hB))
        return hA == hB;
    return StrVal(hA) == StrVal(hB);
}

//O(1) zero-copy concatenation: content-blind, so never interned (intern
//sites are content-aware mints only). Null sides stay 0 and contribute
//nothing at flatten time.
//Precondition callers must uphold (OP_Concat_str guards it): left != right.
//A shared child is not a cycle — edges only point at older nodes, and a
//live parent keeps its children alive, so no true cycle can form — but
//the shared subtree is revisited once per occurrence, and repeated
//self-doubling compounds that into 2^n visits. The defense is
//asymmetric: MarkString is immune (mark bits skip revisits), FlattenInto
//is not, so the guard lives at the semantic layer where the aliasing is
//created.
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
    //K7: the in-place cons flatten rewrites the string store. This
    //guard covers the flatten only — the returned reference is the
    //caller's responsibility (valid while the caller's own guard
    //holds; the minting rule below already pins the same discipline).
    GilEnsureGuard g(*this);
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
//Flat fast path first: most strings are Flat, and the shared walk would
//allocate a work vector per call for them (ffc54fe restored — this
//accessor is execution-path too, via the ReadStrArg family and
//DictKeysEqual, not debug-only).
std::string VmExecutor::StrValCopy(int32_t handle) const {
    if (!IsLiveStringHandle(handle))
        return std::string();
    const StrObj& so = m_stringObjs[static_cast<size_t>(handle)];
    if (so.form != StrObj::Form::Cons)
        return so.str;
    std::string flat;
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
    //K8: sweep runs quiescent at the GC rendezvous — the one writer
    //allowed to touch every shard. ClearAll, then redistribute ALL free
    //indices (newly dead AND residual frees) by the block mapping. The
    //old clear-and-skip loop silently dropped residual frees at every
    //collection (monotone growth leak).
    m_strShards.ClearAll();
    auto strSweepPush = [this](int32_t idx) {
        m_strShards.Push(m_strShards.SweepShardOf(idx), idx);
    };
    size_t liveCount = 0;
    for (size_t i = 1; i < m_stringObjs.size(); ++i) {
        StrObj& so = m_stringObjs[i];
        if (static_cast<uint8_t>(so.form) == kStrFormDead) {
            strSweepPush(static_cast<int32_t>(i));   //residual free stays free
            continue;
        }
        if (so.immortal || m_strMarkBits[i]) {
            ++liveCount;   //immortals are store residents (conservative)
            continue;
        }
        //Weak-intern purification: drop the dying object's table entry so
        //a later mint of the same content allocates fresh instead of
        //resurrecting a dead (or slot-reused) handle.
        if (so.interned)
            m_shortStrTable.erase(so.str);
        so = StrObj{};   //release the buffer
        so.form = static_cast<StrObj::Form>(kStrFormDead);
        strSweepPush(static_cast<int32_t>(i));
    }
    //Pin the no-stale-bits invariant at both ends: MarkPhase's head assign
    //already guarantees a clean vector, and slot handout (TakeStringSlot)
    //never sets mark bits — clearing here makes the invariant hold
    //after every collection, not only before every mark.
    m_strMarkBits.assign(m_stringObjs.size(), false);

    //K6 D3: symmetric with SweepPhase's heap backoff — floor the trigger
    //at the high-water size (the bare 2*live form stuck below it in
    //small-live churn) and at twice the live population (the bare size
    //form collects on every growth allocation once the free list
    //empties — quadratic for monotonically growing chains). The backoff
    //runs unconditionally — stress clamps only the initial thresholds
    //(pre-K6 stress semantics; the deep-chain e2e pins the timing).
    m_strGcThreshold = std::max(m_strGcThreshold,
        std::max(m_stringObjs.size(), 2 * liveCount));
}

size_t VmExecutor::LiveStringObjectCount() const {
    size_t liveCount = 0;
    for (size_t i = 0; i < m_stringObjs.size(); ++i)
        if (static_cast<uint8_t>(m_stringObjs[i].form) != kStrFormDead) ++liveCount;
    return liveCount;
}

} // namespace nlang
