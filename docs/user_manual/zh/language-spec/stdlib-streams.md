# 流——ByteStream与FileStream

两个内建流类提供二进制序列化：`ByteStream`在内存缓冲区上读写，`FileStream`落到磁盘文件。方法面相同（FileStream没有`reset()`）。

```nlang
ByteStream bs = new ByteStream();
bs.writeInt(1);
bs.writeDouble(0.5);
bs.reset();                  // 回卷游标，保留缓冲区
double d = bs.readDouble();
```

| 方法 | 线上形式 | 说明 |
|---------|-----------|-------|
| writeInt / readInt | 4字节 | int32 |
| writeFloat / readFloat | 4字节 | float |
| writeLong / readLong | 8字节 | long（窄整型实参隐式加宽进入） |
| writeDouble / readDouble | 8字节 | double（float实参无损加宽） |
| writeString / readString | 长度前缀 + 字节 | string |
| writeStruct / readStruct | 递归字段 | 写入取struct值；读取取类型名——`bs.readStruct("Point")` |
| writeObject / readObject | 递归引用图 | class实例；读取同样按类型名 |
| length / position | — | 字节数 / 当前游标 |
| reset（仅ByteStream） | — | 回卷游标，保留缓冲区 |
| close | — | 释放FileStream的文件句柄 |

**FileStream构造**：`new FileStream(path, mode)`，mode是`"w"`（创建/截断）、`"a"`（创建/追加）或`"r"`（只读；文件不存在抛运行期错误）。非法mode抛运行期错误。

**实参类型**：标量写入方法按转换矩阵逐实参审查——窄整型与`float`实参隐式加宽进入8字节槽（与`math.sqrt`实参同一规则）；`ulong`实参超出long值域，须显式`as long`；`char`与string实参对数值方法是编译错误。

**流结束（EOS，end of stream）语义**：在游标已到末尾的流上读取抛运行期错误（不是返回0）。
