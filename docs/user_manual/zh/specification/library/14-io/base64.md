> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 14.8 Base64

| 函数 | 含义 |
| --- | --- |
| `io.base64Encode(byte[] data)` | RFC 4648标准字母表（`A-Z a-z 0-9 + /`），带`=`填充。 |
| `io.base64Decode(string s)` | 解码标准Base64；输入长度必须是4的倍数。畸形输入（字母表外字符、长度错误、填充位非零）抛`Exception`。 |
| `io.base64UrlEncode(byte[] data)` | URL安全字母表（`- _`），无填充（JWT风格）。 |
| `io.base64UrlDecode(string s)` | 解码URL安全Base64，填充可有可无；长度≡1 (mod 4)为畸形。 |

```nlang
import io;

int main() {
    byte[] data = io.toBytes("hello");
    io.print(io.base64Encode(data));
    return 0;
}
```
