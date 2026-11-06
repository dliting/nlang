# 20.3 The UdpSocket Class

UDP (User Datagram Protocol) is the connectionless, unordered,
best-effort datagram protocol: each send is one datagram, each
receive takes one whole datagram, and delivery is not guaranteed.
`UdpSocket` covers both roles — an unconnected socket sends and
receives with explicit addresses (`sendTo` / `recvFrom`), and a
connected socket uses plain `send` / `recv` with one peer. The
parking and timeout convention and the one-operation-per-direction
rule are §20.1.

| Member | Shape | Semantics |
|---|---|---|
| `UdpSocket.bind(host = "0.0.0.0", port = 0)` | static → `UdpSocket` | Binds a local endpoint; port `0` lets the system assign one, readable back through `localPort()` |
| `sendTo(data, host, port, count = -1)` | → `int` | Sends one datagram — all of `data`, or its first `count` bytes — to `host:port` |
| `recvFrom(buf, out peerAddress, out peerPort, timeoutMs = 0)` | → `int` | Takes the next datagram and reports its sender through the `out` parameters; returns the received byte count; parks until one arrives or the deadline passes |
| `connect(host, port)` | — | Associates the socket with one peer; afterwards `send` / `recv` need no address and only that peer's datagrams arrive |
| `send(data, count = -1)` | → `int` | Sends one datagram to the associated peer; raises `SocketException` when the socket is unconnected |
| `recv(buf, timeoutMs = 0)` | → `int` | Takes the next datagram from the associated peer and returns its byte count — `0` is an empty datagram, not end of stream: UDP has no stream to end |
| `setBroadcast(on)` | — | Allows sending to the broadcast address (`SO_BROADCAST`) |
| `joinMulticast(group)` / `leaveMulticast(group)` | — | Joins or leaves a multicast group; the receiving interface is the socket's bound address (a wildcard bind selects the system default interface) |
| `setMulticastTtl(ttl)` | — | Time-to-live for outgoing multicast datagrams (`1` stays on the local link) |
| `setTtl(ttl)` | — | Time-to-live for outgoing unicast datagrams |
| `localAddress()` / `localPort()` | → `string` / `int` | The local endpoint |
| `peerAddress()` / `peerPort()` | → `string` / `int` | The associated peer; raise `SocketException` when the socket is unconnected |
| `close()` | — | Closes the socket; idempotent |

## Datagrams, not streams

A received datagram is copied into `buf` and truncated to
`buf.length` when it is longer — the remaining bytes are discarded,
the standard behavior of the sockets interface. Size the receive buffer for the
largest datagram the protocol allows, and use the return value as
the received length: convert or forward only that many bytes.

The request is already queued when `recvFrom` runs, so this exchange
needs no second thread; a real server parks in `recvFrom` and
answers each sender from the `out` parameters:

```nlang
import io;
import net;

int main() {
    UdpSocket srv = UdpSocket.bind("127.0.0.1", 0);
    UdpSocket cli = UdpSocket.bind("127.0.0.1", 0);
    cli.sendTo(io.toBytes("time?"), "127.0.0.1", srv.localPort());
    byte[] buf = byte[64];
    string from;
    int port = 0;
    int n = srv.recvFrom(buf, out from, out port, 5000);
    cli.close();
    srv.close();
    if (n == 5 && from == "127.0.0.1") { return 7; }
    return 1;
}
```

The shipped `examples/net_udp_echo.n` shows the server shape: park,
read the sender's address and port from the `out` parameters, and
echo the received bytes back to exactly that endpoint.
