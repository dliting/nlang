/* ------------------------------------------------------------------
 * gzip_native.cpp — native gzip codec module (spec D21/D23).
 * Two blocking-free entries: compress and decompress byte arrays via
 * vendored zlib. Malformed/truncated input returns a null handle; the
 * script wrapper in gzip.n raises GzipException (Raise cannot name
 * script-declared classes).
 * ------------------------------------------------------------------ */
#include <nlang/vm/NativeHost.h>

#include <cstring>
#include <vector>

#include "zlib.h"

using nlang::native::ArgArray;
using nlang::native::NewArray;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace {

// Copy a script byte[] argument (view, any stride) into a host buffer.
// Returns false and raises IOException when the argument is not a 1-D
// byte array.
bool LoadBytes(NativeHost* h, const uint8_t* a,
               std::vector<unsigned char>& out) {
    NativeArrayView view;
    ArgArray(h, a, 0, &view);
    if (view.rank != 1 ||
        (view.elemKind != NEK_Byte && view.elemKind != NEK_UByte)) {
        Raise(h, NEXC_IOException, "gzip: expected byte[] argument");
        return false;
    }
    const int32_t* cells = static_cast<const int32_t*>(view.data);
    const int64_t o = view.offset;
    const int64_t step = view.strides[0];
    out.resize(static_cast<size_t>(view.elemCount));
    for (int64_t i = 0; i < view.elemCount; ++i) {
        out[static_cast<size_t>(i)] =
            static_cast<unsigned char>(cells[o + i * step] & 0xFF);
    }
    return true;
}

// Return a host buffer as a fresh script byte[]; returns handle 0 (null)
// on allocation failure so the script wrapper can tell.
void ReturnBytes(NativeHost* h, uint8_t* ret,
                 const unsigned char* data, size_t len) {
    const int32_t handle = NewArray(h, NEK_Byte, static_cast<int>(len));
    NativeArrayView view;
    h->getArrayView(h, handle, &view);
    int32_t* cells = static_cast<int32_t*>(view.data);
    for (size_t i = 0; i < len; ++i) {
        cells[i] = data[i];
    }
    ReturnInt(ret, handle);
}

void GzipCompress(NativeHost* h, uint8_t* ret, const uint8_t* a,
                  int argc) {
    std::vector<unsigned char> in;
    if (!LoadBytes(h, a, in)) {
        return;
    }
    uLong bound = compressBound(static_cast<uLong>(in.size())) + 128;
    std::vector<unsigned char> out(bound);
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // windowBits 15+16 = gzip container (RFC 1952), default level.
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16,
                     8, Z_DEFAULT_STRATEGY) != Z_OK) {
        Raise(h, NEXC_IOException, "gzip: deflateInit2 failed");
        return;
    }
    zs.next_in = in.data();
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        Raise(h, NEXC_IOException, "gzip: deflate failed");
        return;
    }
    ReturnBytes(h, ret, out.data(), zs.total_out);
}

void GzipDecompress(NativeHost* h, uint8_t* ret, const uint8_t* a,
                    int argc) {
    std::vector<unsigned char> in;
    if (!LoadBytes(h, a, in)) {
        return;
    }
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // windowBits 15+32 = auto-detect gzip or zlib header.
    if (inflateInit2(&zs, 15 + 32) != Z_OK) {
        Raise(h, NEXC_IOException, "gzip: inflateInit2 failed");
        return;
    }
    std::vector<unsigned char> out;
    unsigned char scratch[16384];
    zs.next_in = in.data();
    zs.avail_in = static_cast<uInt>(in.size());
    int rc = Z_OK;
    // RFC 1952 §2.2 allows concatenated gzip members; inflate() stops at
    // the first Z_STREAM_END (zlib.h: the state "must" be reset to
    // continue), so decode members one by one like gunzip/gzread/Go.
    // Truncated or corrupt input and garbage after the last member both
    // leave rc != Z_STREAM_END and reject the whole input.
    do {
        do {
            zs.next_out = scratch;
            zs.avail_out = sizeof(scratch);
            rc = inflate(&zs, Z_NO_FLUSH);
            if (rc == Z_NEED_DICT || rc == Z_DATA_ERROR || rc == Z_MEM_ERROR) {
                break;
            }
            out.insert(out.end(), scratch,
                       scratch + sizeof(scratch) - zs.avail_out);
        } while (rc != Z_STREAM_END && zs.avail_in != 0);
        if (rc != Z_STREAM_END || zs.avail_in == 0) {
            break;
        }
        if (inflateReset(&zs) != Z_OK) {
            rc = Z_ERRNO;
            break;
        }
    } while (true);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        // Truncated or corrupt: null handle, script wrapper raises.
        ReturnInt(ret, 0);
        return;
    }
    ReturnBytes(h, ret, out.data(), out.size());
}

}  // namespace

NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "gzip", "_gzipCompress", &GzipCompress);
    reg(registry, "gzip", "_gzipDecompress", &GzipDecompress);
    return NLANG_HOST_ABI_VERSION;
}
