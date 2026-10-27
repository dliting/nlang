#pragma once

// NLang runtime is built as a static library, not a DLL.
#define NLANG_RUNTIME_API

#include <cstdint>

namespace nlang {

//Language cap on array rank (T[,] .. up to 64 dims): the grammar, the
//sized-creation resolver and the VM heap layout all read this one
//constant (compiler code cannot include the VM header it lived in).
//64 follows the ecosystem ceiling (NumPy 2.0 / PyTorch NPY_MAXDIMS) so
//the cap itself never becomes the next migration.
inline constexpr uint8_t kMaxArrayRank = 64;

} //namespace nlang
