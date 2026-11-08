# 14.8 Base64

| Function | Meaning |
| --- | --- |
| `io.base64Encode(byte[] data)` | RFC 4648 standard alphabet (`A-Z a-z 0-9 + /`) with `=` padding. |
| `io.base64Decode(string s)` | Decode standard Base64; the input length must be a multiple of 4. Malformed input — a character outside the alphabet, a wrong length, or non-zero padding bits — raises `Exception`. |
| `io.base64UrlEncode(byte[] data)` | URL-safe alphabet (`- _`) without padding, JWT style. |
| `io.base64UrlDecode(string s)` | Decode URL-safe Base64 with or without padding; a length of 1 mod 4 is malformed. |

```nlang
import io;

int main() {
    byte[] data = io.toBytes("hello");
    io.print(io.base64Encode(data));
    return 0;
}
```
