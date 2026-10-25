#pragma once

// NLang runtime is built as a static library, not a DLL.
#define NLANG_RUNTIME_API

#include <cstdint>

namespace nlang {

//Language cap on array rank (T[,] .. T[,,,,,,,]): the grammar, the
//sized-creation resolver and the VM heap layout all read this one
//constant (compiler code cannot include the VM header it lived in).
inline constexpr uint8_t kMaxArrayRank = 8;

} //namespace nlang
