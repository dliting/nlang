/*--- VmClassLayout: the runtime view of per-class instance field layout.

   Consumers (allocation, GC marking, serialization, debugger formatting,
   host field access) read a class's instance fields through this view
   instead of walking the CompiledClass parallel arrays directly. The
   view is owned by VmExecutor and rebuilt whenever InitializeForRun
   binds a module.

   0.9.12 T1: Build mirrors each record's own flattened field arrays, so
   runtime behavior is bit-for-bit unchanged. T2 replaces the Build body
   with a base-first walk of the superClassIdx chain (own-only wire
   records, NCU v2.7); the consumer API below does not change.

   Entry::name borrows the record's string — the module outlives the
   view (both are reset together on the next InitializeForRun).
---*/
#ifndef NLANG_VM_CLASS_LAYOUT_H
#define NLANG_VM_CLASS_LAYOUT_H

#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

struct CompiledClass;

class VmClassLayout {
public:
    // One instance field: declared name plus the RTK kind and the
    // nested-type indices (0xFFFF when the field is not a struct/class).
    struct Entry {
        const std::string* name;
        uint16_t kind;
        uint16_t structIdx;
        uint16_t classIdx;
    };

    void Build(const std::vector<CompiledClass>& classes);

    // Field order defines the heap layout: field i lives at cell 1 + 2i.
    // Callers pass an already-validated classIdx (the GC reads class
    // indices from heap cells with the same trust level today).
    const std::vector<Entry>& Fields(size_t classIdx) const { return m_perClass[classIdx]; }

private:
    std::vector<std::vector<Entry>> m_perClass;
};

} // namespace nlang

#endif // NLANG_VM_CLASS_LAYOUT_H
