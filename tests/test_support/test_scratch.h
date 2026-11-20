/*---
test_scratch.h - scratch directories for the unit tests, one per process.

Tests that compile or load real .n sources write them somewhere writable,
and a name shared by two test processes means both write the same files:
this worktree's ctest running while another worktree's does, or a manual
rerun beside a gate run. Each of them then compiles sources the other one
half-overwrote, and the failures land in whatever test happened to be
reading at that moment. Giving every process its own scratch subtree under
the system temp dir removes that coupling; the named directories stay
stable for the whole run, because the site that writes a project tree and
the site that compiles it are different call sites in the same process.

Leftovers of a finished run are left in place (they cost disk space and help
post-mortem debugging); the per-process root is wiped when it is created, so
a recycled process id cannot hand a new run an old run's files.
---*/
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #include <process.h>
#else
    #include <unistd.h>
#endif

namespace nlang
{
namespace test
{

// Identifies this process, and names the scratch root's last component.
// Process ids are unique among the processes running on a machine, so two
// live test processes cannot share a scratch root.
inline std::string ProcessTag()
{
#if defined(_WIN32)
    return std::to_string(static_cast<unsigned long>(_getpid()));
#else
    return std::to_string(static_cast<unsigned long>(getpid()));
#endif
}

// <temp>/nlang_tests/<ProcessTag()>, created on first use. Anything already
// there belongs to an earlier process that held this id and is removed.
inline const std::filesystem::path& ScratchRoot()
{
    static const std::filesystem::path root = []
    {
        const std::filesystem::path dir
            = std::filesystem::temp_directory_path() / "nlang_tests" / ProcessTag();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        if (!std::filesystem::is_directory(dir))
            throw std::runtime_error("cannot create scratch root: " + dir.string());
        return dir;
    }();
    return root;
}

// A named scratch directory inside ScratchRoot(): empty on first use, then
// the same path with its contents on every later call. Names carry the
// suite that uses them (…_tests), which keeps a temp tree traceable to the
// test that produced it.
inline std::filesystem::path ScratchDir(const std::string& rName)
{
    const std::filesystem::path dir = ScratchRoot() / rName;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir))
        throw std::runtime_error("cannot create scratch dir: " + dir.string());
    return dir;
}

} // namespace test
} // namespace nlang
