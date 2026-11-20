> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 22. url包

`url`包把RFC 3986 URL实现为不可变值类型，外加HTTP客户端与服务器需要的部件编解码。整包纯`nlang`，无`native`代码。

## 22.1 Url.parse与部件字段

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

`Url.parse(reference, maxBytes = 0)`是唯一构造入口。`maxBytes > 0`限定输入长度；超长或语法错误抛`UrlException`。字段遵循RFC部件模型：

| 字段 | 说明 |
| --- | --- |
| `scheme` | 解析时小写化；缺省为`""`。 |
| `userInfo` | 原样字节，不做百分号解码；缺省为`""`。 |
| `host` | 小写化；IPv6字面量保留方括号（`[::1]`）。 |
| `port` | 未给出为`-1`。 |
| `path` | 原样保留；`resolve`之外不做点段移除。 |
| `query` | 无`?`为`""`；`hasQuery`区分「缺失」与「存在但为空」（`http://h?`的`hasQuery`为真且`query`为空）。 |
| `fragment` | 同`hasFragment`的存在性规则。 |

相对引用（无scheme）同样可以解析——`resolve`需要它——但HTTP请求目标必须是绝对的，用`isAbsolute()`检查。

## 22.2 resolve、toString、isAbsolute

`base.resolve(ref)`接受一个引用字符串，执行RFC 3986 §5.2引用解析算法，返回新`Url`；`base`必须是绝对的（否则抛`UrlException`）。`toString()`重组各部件且幂等：解析其输出得到相同的字段值。

## 22.3 查询参数

`queryParams()`把query解码为表单编码键值对的`Dict<string, List<string>>`：重复键的值按序保留在列表里，无`=`的键映射为`""`，畸形百分号转义抛`UrlException`；query缺失或为空时得到空`Dict`。`Url.encodeQuery(params)`是它的逆——请求查询的配对编码器；对`encodeQuery`的输出跑`queryParams`返回同一组配对。

## 22.4 部件编解码

| 函数 | 含义 |
| --- | --- |
| `url.percentEncode(s)` | 把RFC 3986非保留集（`A-Z a-z 0-9 - . _ ~`）之外的每个字节转义为`%XX`——含`/`。约定是「一个完整部件值」：路径段逐段编码后用`/`连接。 |
| `url.percentDecode(s)` | 解码`%XX`转义；`+`不是空格。畸形转义抛`UrlException`。 |
| `url.formEncode(s)` | 同`percentEncode`，但空格变`+`（application/x-www-form-urlencoded）。 |
| `url.formDecode(s)` | 把`+`解码为空格并解码`%XX`转义；畸形转义抛`UrlException`。 |
