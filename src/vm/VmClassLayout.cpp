#include "VmClassLayout.h"

#include "nlang/vm/CompiledModule.h"

namespace nlang {

// T1: mirror each record's flattened field arrays one-for-one, so the
// view exposes exactly the fields (and offsets) consumers read today.
// T2 replaces this loop body with the base-first chain join over the
// own-only wire records; the Entry shape and Fields() API stay put.
void VmClassLayout::Build(const std::vector<CompiledClass>& classes) {
    m_perClass.clear();
    m_perClass.resize(classes.size());
    for (size_t c = 0; c < classes.size(); ++c) {
        const auto& cc = classes[c];
        auto& out = m_perClass[c];
        out.reserve(cc.fieldCount);
        for (uint16_t i = 0; i < cc.fieldCount; ++i) {
            out.push_back(Entry{&cc.fieldNames[i], cc.fieldTypeKinds[i],
                                cc.fieldStructIndices[i], cc.fieldClassIndices[i]});
        }
    }
}

} // namespace nlang
