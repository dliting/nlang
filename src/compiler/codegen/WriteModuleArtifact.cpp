/*---
    WriteModuleArtifact.cpp — VmBackend's artifact-writing member
    (0.13.1a split from VmBackend.cpp: output-path resolution plus the
    single .ncu writer call, separate from the bytecode-generation TU).
---*/
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/core/CompiledModule.h>
#include "VmBackend.h"
#include <filesystem>
#include <fstream>
#include <string>

namespace nlang {

bool VmBackend::WriteModuleArtifact(BuildEnvironment& env,
                                    const CompiledModule& module,
                                    const std::string& entryKey) {
    namespace bf = std::filesystem;
    const BuildParams& setting = env.Params();
    const std::string sFileName = setting.m_sOutputModule + NCU_EXTENSION;

    bf::path modulePath;
    if (setting.m_sOutputDir.empty()) {
        modulePath = bf::current_path() / sFileName;
    } else {
        modulePath = bf::path(setting.m_sOutputDir);
        if (!bf::exists(modulePath)) {
            bf::create_directory(modulePath);
        }
        modulePath /= sFileName;
    }
    const std::string sFilePath = modulePath.string();

    env.Log(CLL_Info, "Output module file: %s ...", sFilePath.c_str());

    std::ofstream fs(sFilePath, std::ios::binary);
    if (!fs.is_open()) {
        env.Log(CLL_Fatal, "Failed to open file: %s.", sFilePath.c_str());
        return false;
    }

    //Serialization lives in WriteCompiledModule (ModuleSaver.cpp) — the
    //single .ncu writer, shared with unit tests so hand-written byte
    //layouts cannot drift from the reader (ModuleLoader).
    if (!WriteCompiledModule(fs, module, entryKey)) {
        env.Log(CLL_Fatal, "Failed to write module: %s.", sFilePath.c_str());
        return false;
    }
    return true;
}

} // namespace nlang
