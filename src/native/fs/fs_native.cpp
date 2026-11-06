// fs native module — the dynamically-loaded implementation of the `fs`
// namespace (names, directories, metadata and the binary whole-file read
// pair; whole-text file content lives in io). Built as nlang_fs.dll /
// libnlang_fs.so and loaded through the same mechanism as a third-party
// native library.
//
// Depends only on the NativeHost ABI, the C++ standard library and
// <filesystem> (used with std::error_code, so no C++ exceptions cross the
// native ABI). Mutating/querying failures raise IOException; the three type
// predicates never raise — a path that cannot be stat simply answers "no".
#include <nlang/vm/NativeHost.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using nlang::native::ArgInt;
using nlang::native::ArgString;
using nlang::native::ReturnInt;
using nlang::native::ReturnString;

namespace {

namespace fsys = std::filesystem;

// DoS bound for the whole-file reads: same 16 MiB cap io.readFile enforces.
constexpr unsigned long long kMaxReadBytes = 16ull * 1024ull * 1024ull;

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

//--- readBytes / readFloats -------------------------------------------------
// Shared whole-file read: open check, size cap and short-read check, with
// every failure mapped to IOException naming the path.
std::string ReadWholeFile(NativeHost* h, const char* funcName,
                          const std::string& path) {
    std::ifstream in(path, std::ios_base::binary);
    if (!in.is_open())
        RaiseIo(h, std::string(funcName) + ": cannot open \"" + path
                + "\".");
    in.seekg(0, std::ios_base::end);
    const std::streamoff size = in.tellg();
    if (size < 0)
        RaiseIo(h, std::string(funcName) + ": cannot determine size of \""
                + path + "\".");
    if (static_cast<unsigned long long>(size) > kMaxReadBytes)
        RaiseIo(h, std::string(funcName) + ": \"" + path
                + "\" is too large to read.");

    std::string bytes(static_cast<size_t>(size), '\0');
    in.seekg(0, std::ios_base::beg);
    if (size > 0)
        in.read(&bytes[0], size);
    if (in.bad() || in.gcount() != static_cast<std::streamsize>(size))
        RaiseIo(h, std::string(funcName) + ": read error on \"" + path
                + "\".");
    return bytes;
}

// A null path is the one string failure that is not an IO failure: the
// program handed no path at all.
void CheckNotNullPath(NativeHost* h, const char* funcName,
                      const uint8_t* a) {
    if (ArgInt(a, 0) == 0)
        nlang::native::Raise(h, NEXC_NullPointer,
                             std::string(funcName) + ": path is null.");
}

void FsReadBytes(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    CheckNotNullPath(h, "fs.readBytes", a);
    const std::string path = ArgString(h, a, 0);
    const std::string bytes = ReadWholeFile(h, "fs.readBytes", path);

    const int32_t count = static_cast<int32_t>(bytes.size());
    const int32_t handle = h->newArray(h, NEK_Int32, count);
    NativeArrayView view;
    h->getArrayView(h, handle, &view);
    // One byte per cell (0..255), not a packed memcpy.
    int32_t* cells = static_cast<int32_t*>(view.data);
    for (size_t i = 0; i < bytes.size(); ++i)
        cells[i] = static_cast<uint8_t>(bytes[i]);
    ReturnInt(ret, handle);
}

void FsReadFloats(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    CheckNotNullPath(h, "fs.readFloats", a);
    const std::string path = ArgString(h, a, 0);
    const std::string bytes = ReadWholeFile(h, "fs.readFloats", path);

    if (bytes.size() % 4 != 0)
        nlang::native::Raise(h, NEXC_Base, "fs.readFloats: \"" + path
                + "\" holds " + std::to_string(bytes.size())
                + " bytes, not a multiple of four.");

    const int32_t count = static_cast<int32_t>(bytes.size() / 4);
    const int32_t handle = h->newArray(h, NEK_Float32, count);
    NativeArrayView view;
    h->getArrayView(h, handle, &view);
    float* cells = static_cast<float*>(view.data);
    for (int32_t i = 0; i < count; ++i) {
        // Assemble the float32 bit pattern from little-endian bytes, so
        // the decode does not depend on the host's byte order.
        const uint32_t bits =
            static_cast<uint32_t>(static_cast<uint8_t>(bytes[4 * i + 0]))
            | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[4 * i + 1])) << 8)
            | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[4 * i + 2])) << 16)
            | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[4 * i + 3])) << 24);
        std::memcpy(&cells[i], &bits, sizeof(float));
    }
    ReturnInt(ret, handle);
}

} // namespace

// Module entry point. Everything touches the filesystem (unbounded
// latency) and registers through the blocking channel — except join,
// pure string composition on already-fetched arguments.
NLANG_DEFINE_NATIVE_INIT {
    regBlocking(registry, "fs", "exists",      &FsExists);
    regBlocking(registry, "fs", "isFile",      &FsIsFile);
    regBlocking(registry, "fs", "isDirectory", &FsIsDirectory);
    regBlocking(registry, "fs", "size",        &FsSize);
    regBlocking(registry, "fs", "listFiles",   &FsListFiles);
    regBlocking(registry, "fs", "makeDirs",    &FsMakeDirs);
    regBlocking(registry, "fs", "remove",      &FsRemove);
    regBlocking(registry, "fs", "readBytes",   &FsReadBytes);
    regBlocking(registry, "fs", "readFloats",  &FsReadFloats);
    reg(registry, "fs", "join",        &FsJoin);
    return NLANG_HOST_ABI_VERSION;
}
