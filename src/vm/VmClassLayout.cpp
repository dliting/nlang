#include "VmClassLayout.h"

#include "nlang/vm/CompiledModule.h"
#include <stdexcept>

namespace nlang {

// 0.9.12 T2 (NCU v2.7): records carry OWN fields only; the view joins
// the superClassIdx chain base-first, once per class. The joined order
// matches the pre-v2.7 flattened records exactly, so every heap offset
// stays put. The depth bound is the class count — a chain that long
// must repeat a class, i.e. a cycle. The loader validated termination,
// acyclicity and index ranges at load time (T2.7); this throw is the
// defensive tripwire for hand-built or corrupt tables.
void VmClassLayout::Build(const std::vector<CompiledClass>& classes) {
    m_perClass.clear();
    m_perClass.resize(classes.size());
    for (size_t c = 0; c < classes.size(); ++c) {
        //Collect the chain (derived → base), then emit base-first. The
        //walk stays in size_t: the loader's class-count ceiling exceeds
        //int16_t range, and a wrapped start index would silently skip
        //the join instead of hitting the tripwire below.
        std::vector<size_t> chain;
        size_t cur = c;
        while (true) {
            if (cur >= classes.size() || chain.size() >= classes.size())
                throw std::runtime_error(
                    "NLang runtime: corrupt class super chain");
            chain.push_back(cur);
            int16_t link = classes[cur].superClassIdx;
            if (link < 0)
                break;
            cur = static_cast<size_t>(link);
        }
        auto& out = m_perClass[c];
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto& cc = classes[*it];
            out.reserve(out.size() + cc.fieldCount);
            for (uint16_t i = 0; i < cc.fieldCount; ++i) {
                out.push_back(Entry{&cc.fieldNames[i], cc.fieldTypeKinds[i],
                                    cc.fieldStructIndices[i], cc.fieldClassIndices[i]});
            }
        }
    }
}

} // namespace nlang
