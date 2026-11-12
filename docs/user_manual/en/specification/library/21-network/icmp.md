# 21.4 The Internet Control Message Protocol (ICMP) Surface

ICMP (Internet Control Message Protocol) echo — the `ping` tool's
protocol — answers what the Transmission Control Protocol (TCP)
surface cannot: whether a host is reachable at all, and how long a
round trip takes. The package
offers it twice: the one-call `net.ping` for reachability and
latency, and `IcmpSocket` for programs that build or read complete
ICMP messages themselves.

## net.ping

| Member | Shape | Semantics |
|---|---|---|
| `net.ping(host, timeoutMs = 5000)` | → `int` | Sends ICMP echo requests to `host` until one is answered — a request that draws no reply is retransmitted until the deadline, the shape the `ping` tool uses; returns the round-trip time in milliseconds; raises `TimeoutException` when the deadline passes with no answer and `SocketException` when the host cannot be resolved or reports unreachable |

`ping` parks the calling context while it waits (§21.1). On Windows
it runs through `IcmpSendEcho2` and needs no administrator rights;
on Linux it uses a datagram ICMP socket, which an unprivileged user
may open once `net.ipv4.ping_group_range` covers that user or group.

A `timeoutMs` of `0` sets no deadline: echo requests keep coming
until one is answered, so a host that never answers leaves `ping`
waiting indefinitely. Each echo request runs on the host's native
worker pool (§27.5): one that carries a deadline is queued at high
priority, one with no deadline at normal priority, since a wait of
unbounded length has no latency of its own to protect. That queue
order is why a positive timeout is the right choice for an address
that may be down — such a ping answers inside its own budget instead
of sitting in the ordinary queue.

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

| Member | Shape | Semantics |
|---|---|---|
| `IcmpSocket.bind(host = "0.0.0.0")` | static → `IcmpSocket` | Opens an ICMP socket bound to `host` |
| `sendTo(icmpMessage, host)` | → `int` | Sends one complete ICMP message — type, code and checksum included — to `host`; on a Windows raw socket a zero checksum field is computed for you |
| `recvFrom(buf, out peerAddress, timeoutMs = 0)` | → `int` | Takes the next ICMP message and reports its source address through the `out` parameter; returns the received byte count; parks until one arrives or the deadline passes |
| `close()` | — | Closes the socket; idempotent |

`IcmpSocket` speaks raw ICMP: the caller assembles the message
bytes, including the checksum. On Windows the raw socket requires
administrator rights; on Linux the unprivileged datagram form is
used, under the same `net.ipv4.ping_group_range` gate as `net.ping`.
A closed `IcmpSocket` follows the §21.1 closed-handle rule:
`sendTo` and `recvFrom` raise `SocketException`. For plain
reachability checks prefer `net.ping`.
