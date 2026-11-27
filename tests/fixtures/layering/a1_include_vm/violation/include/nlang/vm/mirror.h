// Fixture: a vm public header reaching the compiler public
// surface -- every ABI-facing consumer of this tree compiles
// what it includes, so the AST must not leak through the
// public face either.
#include "nlang/compiler/SnExpressions.h"
