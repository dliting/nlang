// fs native module — the dynamically-loaded implementation of the `fs`
// namespace (names, directories and metadata; whole-file content lives in
// io). Built as nlang_fs.dll / libnlang_fs.so and loaded through the same
// mechanism as a third-party native library.
//
// Depends only on the NativeHost ABI, the C++ standard library and
// <filesystem> (used with std::error_code, so no C++ exceptions cross the
// native ABI). Mutating/querying failures raise IOException; the three type
// predicates never raise — a path that cannot be stat simply answers "no".
#include <nlang/vm/NativeHost.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using nlang::native::ArgString;
using nlang::native::ReturnInt;
using nlang::native::ReturnString;

namespace {

namespace fsys = std::filesystem;

void RaiseIo(NativeHost* h, const std::string& msg) {
    nlang::native::Raise(h, NEXC_IOException, msg);
}

//--- type predicates (never raise) -----------------------------------------
void FsExists(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    std::error_code ec;
    bool ok = fsys::exists(ArgString(h, a, 0), ec);
    ReturnInt(ret, ok ? 1 : 0);
}
void FsIsFile(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    std::error_code ec;
    bool ok = fsys::is_regular_file(ArgString(h, a, 0), ec);
    ReturnInt(ret, ok ? 1 : 0);
}
void FsIsDirectory(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    std::error_code ec;
    bool ok = fsys::is_directory(ArgString(h, a, 0), ec);
    ReturnInt(ret, ok ? 1 : 0);
}

//--- size -------------------------------------------------------------------
void FsSize(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    const std::string path = ArgString(h, a, 0);
    std::error_code stEc;
    fsys::file_status st = fsys::status(path, stEc);
    if (st.type() == fsys::file_type::none)
        RaiseIo(h, "fs.size: cannot stat \"" + path + "\": "
                + stEc.message() + ".");
    if (!fsys::exists(st))
        RaiseIo(h, "fs.size: \"" + path + "\" does not exist.");
    if (!fsys::is_regular_file(st))
        RaiseIo(h, "fs.size: \"" + path + "\" is not a regular file.");

    std::error_code szEc;
    std::uintmax_t bytes = fsys::file_size(path, szEc);
    if (szEc)
        RaiseIo(h, "fs.size: cannot stat \"" + path + "\": "
                + szEc.message() + ".");
    if (bytes > static_cast<std::uintmax_t>(INT32_MAX))
        RaiseIo(h, "fs.size: \"" + path + "\" is too large for int.");
    ReturnInt(ret, static_cast<int32_t>(bytes));
}

//--- listFiles --------------------------------------------------------------
void FsListFiles(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    const std::string path = ArgString(h, a, 0);
    std::error_code itEc;
    fsys::directory_iterator it(path, itEc);
    if (itEc)
        RaiseIo(h, "fs.listFiles: cannot open \"" + path + "\": "
                + itEc.message() + ".");

    // Names only (no directory prefix), regular files only, sorted so the
    // unspecified OS iteration order does not affect the result.
    std::vector<std::string> names;
    fsys::directory_iterator end;
    while (it != end) {
        std::error_code typeEc;
        if (it->is_regular_file(typeEc))
            names.push_back(it->path().filename().generic_string());
        it.increment(itEc);
        if (itEc)
            RaiseIo(h, "fs.listFiles: iteration error in \"" + path + "\": "
                    + itEc.message() + ".");
    }
    std::sort(names.begin(), names.end());

    // Build the List<string> through the host; the c_str pointers stay valid
    // for the duration of this one synchronous call.
    std::vector<const char*> ptrs;
    ptrs.reserve(names.size());
    for (const std::string& n : names)
        ptrs.push_back(n.c_str());
    ReturnInt(ret, h->newListString(h, ptrs.data(),
                                    static_cast<int>(ptrs.size())));
}

//--- makeDirs / remove ------------------------------------------------------
void FsMakeDirs(NativeHost* h, uint8_t*, const uint8_t* a, int) {
    const std::string path = ArgString(h, a, 0);
    std::error_code ec;
    fsys::create_directories(path, ec);  // mkdir -p; existing dir is success
    if (ec)
        RaiseIo(h, "fs.makeDirs: cannot create \"" + path + "\": "
                + ec.message() + ".");
}
void FsRemove(NativeHost* h, uint8_t*, const uint8_t* a, int) {
    const std::string path = ArgString(h, a, 0);
    std::error_code ec;
    fsys::remove(path, ec);  // file or empty dir; missing path is a no-op
    if (ec)
        RaiseIo(h, "fs.remove: cannot remove \"" + path + "\": "
                + ec.message() + ".");
}

//--- join -------------------------------------------------------------------
void FsJoin(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    const std::string x = ArgString(h, a, 0);
    const std::string y = ArgString(h, a, 1);
    std::string joined = (fsys::path(x) / y).generic_string();
    ReturnString(h, ret, joined);
}

} // namespace

// Module entry point.
NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "fs", "exists",      &FsExists);
    reg(registry, "fs", "isFile",      &FsIsFile);
    reg(registry, "fs", "isDirectory", &FsIsDirectory);
    reg(registry, "fs", "size",        &FsSize);
    reg(registry, "fs", "listFiles",   &FsListFiles);
    reg(registry, "fs", "makeDirs",    &FsMakeDirs);
    reg(registry, "fs", "remove",      &FsRemove);
    reg(registry, "fs", "join",        &FsJoin);
    return NLANG_HOST_ABI_VERSION;
}
