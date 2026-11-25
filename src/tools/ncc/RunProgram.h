/*---
    RunProgram.h — the run half of `ncc run`: layered search assembly,
    closure load with the native-member cache wiring (D5.2), link and
    execute, plus the run-time diagnostics. main() owns the argument
    parse; this owns the execution (the same call sequence as nvm —
    see the note there).
---*/
#pragma once
#include <string>
#include <vector>

namespace nlang
{

//Runs one compiled artifact (.ncu or .npkg). Returns the program's exit
//code, or 1 on a load/link/run failure with the diagnostics printed.
int RunProgramArtifact(const std::string& artifact,
    const std::vector<std::string>& explicitDirs, bool verbose);

} //namespace nlang
