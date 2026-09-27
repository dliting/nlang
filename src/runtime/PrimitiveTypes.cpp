/*--- PrimitiveTypes.cpp - registry data + lookups. ---*/
#include "PrimitiveTypes.h"

namespace nlang
{

#define INFO_ROW(Name, Kw, Width, Carrier, Cat, Rank)                     \
    { NK_##Name, RTK_##Name, Width, Cat, Rank, Kw },
const ScalarPrimInfo kScalarPrims[] =
{
#define MACRO_IMPL INFO_ROW
    SCALAR_PRIMITIVE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
};
const size_t kScalarPrimCount = sizeof(kScalarPrims) / sizeof(kScalarPrims[0]);

int ScalarPrimIndexOf(NodeKind kind)
{
    for (size_t i = 0; i < kScalarPrimCount; ++i)
        if (kScalarPrims[i].kind == kind)
            return static_cast<int>(i);
    return -1;
}

int ScalarPrimIndexOfRtk(uint8_t rtk)
{
    for (size_t i = 0; i < kScalarPrimCount; ++i)
        if (kScalarPrims[i].rtk == rtk)
            return static_cast<int>(i);
    return -1;
}

bool PrimKindIsTraceable(NodeKind kind)
{
    (void)kind;  // scalars never reference the heap
    return false;
}

bool PrimKindIsNumeric(NodeKind kind)
{
    int i = ScalarPrimIndexOf(kind);
    return i >= 0 && PrimCategoryIsNumeric(kScalarPrims[i].category);
}

} // namespace nlang
