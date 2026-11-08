> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 21.4 互联网控制报文协议（ICMP）面

互联网控制报文协议（ICMP，Internet Control Message Protocol）的回声请求——`ping`工具的协议——回答的是传输控制协议（TCP，Transmission Control Protocol）面回答不了的问题：一台主机到底可达不可达，一次往返要多久。这个包把它给了两次：一次调用出结果的`net.ping`用于探可达与测时延，以及`IcmpSocket`给亲自组装、读取完整ICMP消息的程序。

## net.ping

| 成员 | 形态 | 语义 |
|---|---|---|
| `net.ping(host, timeoutMs = 5000)` | →`int` | 向`host`发送ICMP回声请求直到有一个得到应答——没有回音的请求会重传，直到截止时刻到来，这正是`ping`工具的形态；以毫秒返回往返时延；截止时刻到来仍无应答抛`TimeoutException`，主机无法解析或回报不可达抛`SocketException` |

`ping`在等待期间停泊调用上下文（§21.1）。Windows上它走`IcmpSendEcho2`，不需要管理员权限；Linux上它走数据报ICMP套接字，普通用户在`net.ipv4.ping_group_range`覆盖到该用户或组之后即可打开。

```nlang
import io;
import net;

int main() {
    int rtt = net.ping("127.0.0.1", 3000);
    io.print("loopback rtt=" + rtt.toString() + "ms\n");
    return 0;
}
```

## IcmpSocket

| 成员 | 形态 | 语义 |
|---|---|---|
| `IcmpSocket.bind(host = "0.0.0.0")` | 静态→`IcmpSocket` | 打开一个绑定到`host`的ICMP套接字 |
| `sendTo(icmpMessage, host)` | →`int` | 发送一条完整的ICMP消息——类型、代码与校验和都包含在内——到`host`；Windows原始套接字下，校验和字段为0时由库代为计算 |
| `recvFrom(buf, out peerAddress, timeoutMs = 0)` | →`int` | 取走下一条ICMP消息，经`out`参数报告它的源地址；返回实收字节数；停泊直到有消息到来或截止时刻到来 |
| `close()` | — | 关闭套接字；幂等 |

`IcmpSocket`说的是原始ICMP：消息字节由调用方组装，校验和也算在内。Windows上原始套接字需要管理员权限；Linux上用免特权的原始数据报形态，受与`net.ping`相同的`net.ipv4.ping_group_range`约束。只做普通的可达性检查，请用`net.ping`。
