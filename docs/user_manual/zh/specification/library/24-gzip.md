> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 24. gzip包

`gzip`包对整块字节缓冲做gzip格式（RFC 1952，载荷为RFC 1951的DEFLATE编解码）压缩与解压。一次调用一个来回：`compress`取字节、返回一条完整的gzip流，`decompress`取一条gzip流、返回其中的字节。没有流式模式——大文件整体驻留内存——也没有压缩级别或策略旋钮。

本包只有两个入口与一个异常类，而标准库里唯一用它是`http`：服务器的`HttpResponse.compressBody`（§23.5）产出`Accept-Encoding: gzip`客户端所要求的应答，缓冲的客户端路径再把这类应答解回来（§23.6）。

与`url`和`http`不同，本包并非纯`nlang`。两个函数包装了`_gzipCompress`与`_gzipDecompress`这两个`native`成员，实现放在与解释器并排随工具链分发的`nlang_gzip.dll`里，zlib编解码就编译在该模块内（§27.2说明解释器在哪些目录找这类模块）；`native`报告失败时不返回值，也无法抛出本包声明的类，于是脚本层把这一步转成`GzipException`。

## 24.1 compress与decompress

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

程序打印两侧长度，打印被拒绝输入所携带的消息，打印`ok`，只在解压结果与原内容完全一致时以24退出（§9的自检写法）。

| 函数 | 说明 |
| --- | --- |
| `compress(data)` | 为`data`产出一条完整的gzip流，级别取编解码器默认值。本就不小的输入可能反而多出几个字节，这是带头部与完整性校验的封装格式的常态；空输入产出一条合法的空流。 |
| `decompress(data)` | 流中所含的字节。首尾相接的多条gzip流会逐条解出再拼接（RFC 1952 §2.2），且zlib封装的流（RFC 1950）与gzip流一样被接受——尽管`compress`只发gzip。 |

解码是全有或全无。提前截断的流、完整性校验失败的流、以及末条流之后还跟着无处安放的字节的流，整份被拒绝：`decompress`从不交出已经解压出来的那一部分。空输入同样被拒绝，理由一致——那里没有可解的流。这种严格是有意为之：拿到截断字节的调用者与拿到完整正文的调用者否则看起来一模一样，而HTTP响应正是这个差别要命的地方（§23.6）。

## 24.2 GzipException

`GzipException`派生自`IOException`（§8.3、§20），是`const class`，字段一旦构造便不再改变。本包自己定名的失败以`GzipException`抛出，只有两种消息：

| 消息 | 含义 |
| --- | --- |
| `gzip: compression failed` | 编码器拒绝——编解码器侧的失败，与输入内容无关。 |
| `gzip: input is not a valid gzip stream` | 解码器未能产出一条完整的流：起始字节不是可辨认的头部、压缩方法不是gzip定义的那一种、载荷被截断或完整性校验失败、或最后一条流之后还有字节。 |

在来得及给出判定之前就发生的编解码器故障——初始化或压缩步骤被编解码器拒绝——则以普通`IOException`浮出，消息按被拒绝的调用命名：`gzip: deflateInit2 failed`、`gzip: deflate failed`、`gzip: inflateInit2 failed`。异常可以带上`cause`，与其他异常一致（§8.2）。两族都派生自`IOException`，只想知道「压缩失败了」的代码可以捕获`IOException`，不必过问是哪一层报告的。

## 24.3 与http包的关系

服务器通过设置`compressBody`让一个缓冲响应参与压缩；压缩只对接受gzip的客户端发生，并在响应中用`Content-Encoding`与`Vary`说明（§23.5）。缓冲的客户端调用自行加上`Accept-Encoding: gzip`并把答案解压回来，`res.body`里是解压后的字节（§23.6）。

流式调用两样都不做，因为本包唯一的解压函数一次要整块缓冲。流式响应带着`Content-Encoding: gzip`到达时，先收齐正文再在此处解码：

```nlang
byte[] gz = res.bodyReader.readAll();
byte[] plain = gzip.decompress(gz);
```

或者索性索要你打算自己读的编码——在请求上自己设置`Accept-Encoding`，客户端便不碰那些字节（§23.6）。
