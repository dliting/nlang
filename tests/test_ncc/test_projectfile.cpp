// --- ProjectFile (.nproj loader) unit tests ---
// Phase 10 Step 0: ncc -p <project.nproj> support. The loader parses the
// project XML (tinyxml2) and resolves <File path="..."> entries against the
// project file's directory. Fixtures are written to a temp dir so the test
// is self-contained.
#include "ProjectFile.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static fs::path WriteFixture(const fs::path& dir, const char* name,
                             const std::string& content) {
    fs::path p = dir / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

int main() {
    std::printf("=== ncc ProjectFile Unit Tests ===\n\n");
    fs::path tmp = fs::temp_directory_path() / "nlang_projfile_test";
    fs::create_directories(tmp);

    // 1. Valid project: name + two sources, paths resolved against the
    //    project dir, project-file order preserved.
    {
        auto proj = WriteFixture(tmp, "valid.nproj",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"Hello\" namespace=\"hello\" outputDir=\"bin\">\n"
            "  <Sources>\n"
            "    <File path=\"main.n\"/>\n"
            "    <File path=\"sub/utils.n\"/>\n"
            "  </Sources>\n"
            "</Project>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(pf.name == "Hello");
        CHECK(pf.projectDir == tmp.string());
        CHECK(pf.sources.size() == 2);
        CHECK(pf.sources[0] == (tmp / "main.n").string());
        CHECK(pf.sources[1] == (tmp / "sub" / "utils.n").string());
    }

    // 2. Missing project file → error mentions the path.
    {
        nlang::ProjectFile pf;
        std::string err;
        auto missing = (tmp / "no_such.nproj").string();
        CHECK(!nlang::ProjectFile::Load(missing, pf, err));
        CHECK(!err.empty());
    }

    // 3. Malformed XML → parse error, not a crash.
    {
        auto proj = WriteFixture(tmp, "broken.nproj",
            "<Project name='x'><Sources>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(!nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(!err.empty());
    }

    // 4. Wrong root element → error.
    {
        auto proj = WriteFixture(tmp, "wrongroot.nproj",
            "<?xml version=\"1.0\"?>\n<Solution name=\"s\"/>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(!nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(!err.empty());
    }

    // 5. Empty <Sources/> → error (nothing to compile is a user mistake,
    //    not a valid empty build).
    {
        auto proj = WriteFixture(tmp, "empty.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"Empty\">\n  <Sources/>\n</Project>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(!nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(!err.empty());
    }

    // 6. <File/> without path attribute → error naming the entry.
    {
        auto proj = WriteFixture(tmp, "noattr.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"X\">\n  <Sources>\n    <File/>\n  </Sources>\n</Project>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(!nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(!err.empty());
    }

    // 7. outputDir parsed for the caller (ncc composes the output path).
    {
        auto proj = WriteFixture(tmp, "outdir.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"Out\" outputDir=\"bin\">\n"
            "  <Sources><File path=\"main.n\"/></Sources>\n</Project>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(pf.outputDir == "bin");
    }

    // 8. Name defaults to the file stem when the attribute is absent.
    {
        auto proj = WriteFixture(tmp, "noname.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project>\n  <Sources><File path=\"main.n\"/></Sources>\n</Project>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(pf.name == "noname");
    }

    // 9. Duplicate detection is path-normalized: "sub/../main.n" and
    //    "main.n" name the same file and must be rejected.
    {
        auto proj = WriteFixture(tmp, "dupnorm.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"D\">\n  <Sources>\n"
            "    <File path=\"main.n\"/>\n"
            "    <File path=\"sub/../main.n\"/>\n"
            "  </Sources>\n</Project>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(!nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(!err.empty());
    }

    // 10. Absolute <File path="..."> replaces the project dir wholesale.
    {
        fs::path abs = tmp / "abs_main.n";
        WriteFixture(tmp, "abs_main.n", "");
        std::string xml = "<?xml version=\"1.0\"?>\n<Project name=\"A\">\n"
            "  <Sources><File path=\"" + abs.string() + "\"/></Sources>\n"
            "</Project>\n";
        auto proj = WriteFixture(tmp, "absfile.nproj", xml);
        nlang::ProjectFile pf;
        std::string err;
        CHECK(nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(pf.sources.size() == 1);
        CHECK(pf.sources[0] == abs.string());
    }

#ifdef _WIN32
    // 11. Windows: duplicate detection is case-insensitive (NTFS semantics)
    //     — "Main.n" and "main.n" name the same physical file.
    {
        WriteFixture(tmp, "main.n", "");
        auto proj = WriteFixture(tmp, "dupcase.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"C\">\n  <Sources>\n"
            "    <File path=\"Main.n\"/>\n"
            "    <File path=\"main.n\"/>\n"
            "  </Sources>\n</Project>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(!nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(!err.empty());
    }
#endif

    // 12. A second <Sources> block is a schema violation, not silently
    //     ignored (files listed there would be dropped without a trace).
    {
        auto proj = WriteFixture(tmp, "twosources.nproj",
            "<?xml version=\"1.0\"?>\n"
            "<Project name=\"T\">\n  <Sources><File path=\"main.n\"/></Sources>\n"
            "  <Sources><File path=\"extra.n\"/></Sources>\n</Project>\n");
        nlang::ProjectFile pf;
        std::string err;
        CHECK(!nlang::ProjectFile::Load(proj.string(), pf, err));
        CHECK(!err.empty());
    }

    std::printf("\n=== Results: %s (%d failure%s) ===\n",
        g_failures == 0 ? "all passed" : "FAILURES",
        g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
