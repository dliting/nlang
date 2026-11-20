# 22. The url Package

The `url` package implements RFC 3986 URLs as an immutable
value type plus the component codecs HTTP clients and servers
need. It is pure NLang — no native code.

## 22.1 Url.parse and the component fields

```nlang
import io;
import url;

int main() {
    Url u = Url.parse("https://example.com:8443/search?q=nlang#top");
    io.print(u.host + ":" + u.port.toString() + "\n");
    io.print(u.query + "\n");
    return 0;
}
```

`Url.parse(reference, maxBytes = 0)` is the only construction
entry point. `maxBytes > 0` bounds the input length; a longer
reference or a syntax error raises `UrlException`. The fields
follow the RFC component model:

| Field | Notes |
| --- | --- |
| `scheme` | Lowercased at parse time; `""` when absent. |
| `userInfo` | Raw bytes, not percent-decoded; `""` when absent. |
| `host` | Lowercased; IPv6 literals keep their brackets (`[::1]`). |
| `port` | `-1` when not given. |
| `path` | As written; no dot-segment removal outside `resolve`. |
| `query` | `""` when there is no `?`; `hasQuery` tells absent from present-but-empty (`"http://h?"` has `hasQuery` true with `query` empty). |
| `fragment` | The same presence rule through `hasFragment`. |

Relative references (no scheme) parse fine — `resolve` needs
them — but an HTTP request target must be absolute; check with
`isAbsolute()`.

## 22.2 resolve, toString, isAbsolute

`base.resolve(ref)` takes a reference string, applies the
RFC 3986 §5.2 reference-resolution algorithm and returns a new
`Url`; `base` must be absolute (`UrlException` otherwise).
`toString()` reassembles the components and round-trips:
parsing its output yields the same field values.

## 22.3 Query parameters

`queryParams()` decodes the query into a
`Dict<string, List<string>>` of form-encoded pairs: repeated
keys keep their order in the list, a key without `=` maps to
`""`, malformed percent escapes raise `UrlException`, and an
absent or empty query yields an empty `Dict`.
`Url.encodeQuery(params)` is the inverse — the pair encoder for
request queries; `queryParams` of `encodeQuery` output returns
the same pairs.

## 22.4 Component codecs

| Function | Meaning |
| --- | --- |
| `url.percentEncode(s)` | Escape every byte outside the RFC 3986 unreserved set (`A-Z a-z 0-9 - . _ ~`) as `%XX` — `/` included. The contract is one full component value: encode path segments separately and join them with `/`. |
| `url.percentDecode(s)` | Decode `%XX` escapes; `+` is not a space. Malformed escapes raise `UrlException`. |
| `url.formEncode(s)` | Like `percentEncode` but spaces become `+` (application/x-www-form-urlencoded). |
| `url.formDecode(s)` | Decode `+` as space and `%XX` escapes; malformed escapes raise `UrlException`. |
