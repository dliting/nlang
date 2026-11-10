# 24. The gzip Package

The `gzip` package compresses and decompresses a whole byte buffer in
the gzip format (RFC 1952, carrying the DEFLATE codec of RFC 1951).
One call is one round trip: `compress` takes bytes and returns a
complete gzip stream, `decompress` takes a gzip stream and returns the
bytes. There is no streaming mode — a large file is held in memory
whole — and no compression level or strategy knob.

The package has two entry points and one exception class, and
`http` is the standard library's only user of it: the server's
`HttpResponse.compressBody` (§23.5) produces the answers an
`Accept-Encoding: gzip` client asks for, and the buffered client path
decodes such an answer (§23.6).

Unlike `url` and `http`, this package is not pure NLang. The two
functions wrap `_gzipCompress` and `_gzipDecompress`, `native` members
implemented in `nlang_gzip.dll`, the module shipped beside the
interpreter with the zlib codec compiled into it (§27.2 covers where
the interpreter looks for such a module); a `native` reports failure by
returning nothing, and it cannot raise a class this package declares,
so the script layer turns that into `GzipException`.

## 24.1 compress and decompress

```nlang
import io;
import gzip;

int main() {
    string text = "round trip, round trip, round trip.";
    byte[] data = io.toBytes(text);
    byte[] packed = gzip.compress(data);
    byte[] back = gzip.decompress(packed);
    if (io.fromBytes(back) != text) { return 1; }
    io.print("packed " + packed.length().toString() + " of " + data.length().toString());
    try {
        gzip.decompress(io.toBytes("not gzip at all"));
        return 1;
    } catch (GzipException e) {
        io.print("refused: " + e.message);
    }
    io.print("ok");
    return 24;
}
```

The program prints the sizes, prints the message a refused input
carries, prints `ok`, and exits 24 only if the round trip restored the
text exactly (the self-check idiom of §9).

| Function | Notes |
| --- | --- |
| `compress(data)` | A complete gzip stream for `data`, at the codec's default level. Already-small input can come back a few bytes longer, which is normal for a container with a header and an integrity check; empty input yields a valid empty stream. |
| `decompress(data)` | The bytes the stream holds. Several gzip streams concatenated end to end are decoded one after another and joined (RFC 1952 §2.2), and a zlib-wrapped stream (RFC 1950) is accepted alongside a gzip one — though `compress` only ever emits gzip. |

Decoding is all or nothing. A stream that stops short of its end,
fails its integrity check, or is followed by bytes the decoder cannot
place is refused as a whole: `decompress` never hands back the part it
managed to inflate. Empty input is refused for the same reason — there
is no stream there to decode. The strictness is deliberate: a caller
that got truncated bytes and a caller that got a complete body would
otherwise look identical, and an HTTP response is exactly the place
where that difference matters (§23.6).

## 24.2 GzipException

`GzipException` derives from `IOException` (§8.3, §20) and is a `const
class`, so its fields are fixed once constructed. Failures the package
names itself come back as a `GzipException` with one of two messages:

| Message | Meaning |
| --- | --- |
| `gzip: compression failed` | The encoder refused — a codec-side failure rather than anything wrong with the input. |
| `gzip: input is not a valid gzip stream` | The decoder could not produce one complete stream: the leading bytes are not a recognised header, the compression method is not the one gzip defines, the payload is truncated or fails its integrity check, or bytes follow the last stream. |

The rare codec breakdowns that fail before the package can pass a
verdict — an initialization or a compression step the codec refuses —
come up as plain `IOException` instead, named after the refused call:
`gzip: deflateInit2 failed`, `gzip: deflate failed`, `gzip:
inflateInit2 failed`. A `cause` may ride along, as for any exception
(§8.2). Both families derive from `IOException`, so code that only
wants to know that compression went wrong can catch `IOException` and
stay indifferent to which layer reported it.

## 24.3 gzip and the http package

A server opts a buffered response into compression by setting
`compressBody`; the compression then happens only for a client that
accepts gzip, and the response says so with `Content-Encoding` and
`Vary` (§23.5). A buffered client call adds
`Accept-Encoding: gzip` and inflates the answer for you, leaving
`res.body` the uncompressed bytes (§23.6).

A streaming call does neither, because the only decompressor here
wants the whole buffer at once. If a streamed response arrives with
`Content-Encoding: gzip`, collect the body and decode it here:

```nlang
byte[] gz = res.bodyReader.readAll();
byte[] plain = gzip.decompress(gz);
```

Or ask for the encoding you intend to read — set `Accept-Encoding` on
the request yourself, and the client leaves the bytes alone
(§23.6).
