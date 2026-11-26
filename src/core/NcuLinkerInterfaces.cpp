/*---
    NcuLinkerInterfaces.cpp — nlink Pass A的接口表合并
    （NcuLinkerInterfaces.h）。从NcuLinker.cpp拆出（2026-11-03尺寸
    守卫拆分，零行为变化）。
---*/
#include "NcuLinkerInterfaces.h"
#include <algorithm>

namespace nlang
{

namespace
{
//v0.12.6 Task 8: a contentless row a consumer unit pushed for an
//imported interface stub — no initializer record, no static methods, no
//constants, no instance signatures.
bool IsPlaceholderInterfaceRow(const CompiledInterface& ci) {
    return ci.staticInitFuncIdx == 0xFFFF
        && ci.staticMethodIndices.empty()
        && ci.constants.empty()
        && ci.instanceMethods.empty();
}
} //namespace

//0.9.10 v2.4: interface statics, deduped by qualified key like classes.
//No import slots exist for this table (statics never cross as types), so
//every entry is an own entry; the first provider owns the metadata remap.
//v0.12.6 Task 7 (D11): kind-1 class-init gates address this table, so
//the merge records the unit→merged map for every index (deduped or
//pushed) like the class table.
//v0.12.6 Task 8: a consumer unit now also pushes contentless PLACEHOLDER
//rows for imported interface stubs (RegisterInterfaces' imported arm —
//kind-1 gate operands need a unit-local index). The entry unit usually
//merges first, so its placeholder would win the name dedup and silently
//drop the producer's content; the merge therefore prefers the rich row
//IN PLACE (the merged index is preserved, both units' maps keep pointing
//at it). Two rich rows keep the first-wins doctrine (builtins register
//rich in every unit). The in-place swap also moves the metadata-remap
//duty: the replaced row's producer indices must remap through the
//PRODUCER's maps, so the swapped-in row leaves the placeholder pusher's
//set and joins this unit's (Pass C remaps each pushed index with its
//owner unit's maps).
void MergeInterfaces(const CompiledModule& u, CompiledModule& merged,
                    NcuOperandMaps& m,
                    std::vector<UnitPushes>& pushes, size_t ui) {
    for (uint32_t i = 0; i < u.interfaces.size(); ++i) {
        int existing = -1;
        for (size_t mi = 0; mi < merged.interfaces.size(); ++mi)
            if (merged.interfaces[mi].name == u.interfaces[i].name) {
                existing = static_cast<int>(mi);
                break;
            }
        if (existing >= 0) {
            if (IsPlaceholderInterfaceRow(merged.interfaces[existing])
                && !IsPlaceholderInterfaceRow(u.interfaces[i])) {
                merged.interfaces[existing] = u.interfaces[i];
                for (size_t pj = 0; pj < ui; ++pj) {
                    auto& prev = pushes[pj].interfaces;
                    prev.erase(std::remove(prev.begin(), prev.end(),
                        static_cast<uint32_t>(existing)), prev.end());
                }
                pushes[ui].interfaces.push_back(
                    static_cast<uint32_t>(existing));
            }
            m.interfaces[i] = static_cast<uint32_t>(existing);
            continue;
        }
        m.interfaces[i] =
            static_cast<uint32_t>(merged.interfaces.size());
        merged.interfaces.push_back(u.interfaces[i]);
        pushes[ui].interfaces.push_back(m.interfaces[i]);
    }
}

} //namespace nlang
