/*---
    ScriptRunner.h -- in-process compile+execute pipeline for the nlang
    tool (v0.9.1 script mode). Single-header implementation, mirroring
    the ncc SearchPathArgs.h tool-header pattern. The pipeline mirrors
    tests/test_vm/test_perunit.cpp perUnitRun (the only true in-memory
    path): BuildUnitImages() writes nothing to disk; the link closure is
    units + images.external; entryKey comes from the images, never from
    a LoadClosure record round-trip.
---*/
#pragma once
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>   //GetCurrentProcessId (temp file naming)
#else
#include <unistd.h>    //getpid
#endif

namespace nlangtool {

//Outcome of one script run. ok=false means the script never executed
//(compile/link/internal error); the exit code is then the tool's own 1.
struct RunOutcome {
    int exitCode = 1;
    bool ok = false;
};

//RAII carrier for `nlang -c "<statements>"`: the statements become a
//temp .n file so line numbers survive diagnostics (raw UTF-8 content,
//no textual wrapping). Removed on every exit path, including exceptions.
class TempScriptFile {
public:
    explicit TempScriptFile(const std::string& utf8Content) {
        static std::atomic<unsigned> counter{0};
        m_path = std::filesystem::temp_directory_path()
            / ("nlang-c-" + std::to_string(GetProcessId()) + "-"
                + std::to_string(counter++) + ".n");
        std::ofstream out(m_path, std::ios::binary);
        out << utf8Content;
        //Disk-full/permission failure surfaces here; without this the
        //Task 6 pipeline would only report a confusing "cannot open"
        //compile error for a file the tool itself failed to create.
        if (!out)
            std::fprintf(stderr,
                "nlang: cannot create temp script file: %s\n",
                m_path.string().c_str());
    }
    ~TempScriptFile() {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }
    TempScriptFile(const TempScriptFile&) = delete;
    TempScriptFile& operator=(const TempScriptFile&) = delete;
    const std::filesystem::path& Path() const { return m_path; }
private:
    static unsigned GetProcessId() {
#ifdef _WIN32
        return static_cast<unsigned>(GetCurrentProcessId());
#else
        return static_cast<unsigned>(::getpid());
#endif
    }
    std::filesystem::path m_path;
};

//Task 5 skeleton placeholder -- the in-process pipeline (BuildUnitImages
//-> NcuLinker::Link -> VmExecutor) lands in Task 6. Until then the tool
//reports the gap instead of pretending to run.
inline RunOutcome RunScript(const std::filesystem::path&,
                            const std::vector<std::filesystem::path>&) {
    std::fprintf(stderr, "nlang: script execution is not yet wired up\n");
    return {};
}

} //namespace nlangtool
