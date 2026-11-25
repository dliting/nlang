/*---
ModuleBuilderImportCycles.h - internal seam of the import graph: the
one collector of a loaded image's import-record targets, shared by the
compile-time cycle check (ModuleBuilderImportCycles.cpp) and the
discovery fixed point (ModuleBuilderImports.cpp), so both walk the
same five slot tables and cannot disagree about what a mirror imports.
---*/
#pragma once
#include "nlang/vm/CompiledModule.h"
#include <string>
#include <vector>

namespace nlang
{

//Every import record target of a loaded image (empty paths skipped —
//an internal marker, not a module edge).
std::vector<std::string> ImageImportTargets(const CompiledModule &image);

} //namespace nlang
