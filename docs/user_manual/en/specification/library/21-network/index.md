# 21. The Network Library

The network library is one standard-library package, `net`: TCP
(Transmission Control Protocol) streams and listeners, UDP (User Datagram Protocol)
datagrams, and ICMP (Internet Control Message Protocol) echo. The parking
and scheduling semantics of every blocking method are §10.1: a blocked
socket call parks the calling virtual thread and releases its carrier, so
thousands of concurrent connections can wait on sockets at once — the
count is not bounded by the number of carrier threads, and it holds even
with a single carrier. §21.1 fixes the package form, the surface and the
shared conventions; §21.2 defines the TCP classes; §21.3 defines
`UdpSocket`; §21.4 defines the ICMP surface.

## 21.1 The net Package

`net` is a standard library package (§3.9.4): `import net;` brings
it into scope. The package name is single-segment (§3.3). Its
declarations are the standard library's `.n` source at
`stdlib/net.n`; its `native` members (§7.5) are implemented by the
native module `nlang_net` shipped with the toolchain — the same
third-party mechanism as `tensor`'s kernels (§19.1) — loaded lazily
on the first call (§7.5). The virtual machine itself does not
depend on the module: a program that never touches a socket never
loads it. The package declares the classes `TcpListener`,
`TcpStream`, `UdpSocket` and `IcmpSocket`, the free function
`ping`, and the exception subclasses `SocketException` and
`TimeoutException` (§8.3); the classes reach scope through the
import as ordinary classes, and `ping` is called through the
package qualifier (`net.ping(host)`). A user package named `net`
collides with the standard library and is rejected by the
duplicate-package rule (§11.1).

The package surface:

- `net.ping(host, timeoutMs = 5000)` sends ICMP echo requests to
  `host` until one is answered and returns the round-trip time in
  milliseconds (§21.4).
- `TcpListener` — bind, accept, close (§21.2).
- `TcpStream` — connect, read, write, readLine, readExactly,
  shutdownWrite, close, and the option setters (§21.2).
- `UdpSocket` — bind, sendTo, recvFrom, connect, send, recv, and
  the broadcast, multicast and buffer-size options (§21.3).
- `IcmpSocket` — bind, sendTo, recvFrom, close (§21.4).

### Parking and timeouts

Every blocking method — `accept`, `read`, `readLine`,
`readExactly`, `write`, `send`, `sendTo`, `recv`, `recvFrom`,
`connect`, `ping` — parks the calling virtual thread until the
operation completes or the deadline passes; the carrier runs other
contexts meanwhile (§10.1).
Methods that name an address by host name — `bind`, `connect`,
`sendTo`, `ping` — park while the name resolves as well:
resolution runs on the runtime's resolver pool, so a slow lookup
occupies no carrier (§10.1).
The `timeoutMs` parameter fixes how long the call waits: `0` (the
default) waits without a deadline, a positive value waits at most
that many milliseconds. On the calls that name a host — `connect`,
`sendTo` — the deadline covers name resolution too. A wait that
ends at its deadline raises `TimeoutException`; the socket stays
open and usable, and a later call may succeed — the one caveat is a
`write` that times out mid-payload: part of the payload may already
be with the kernel, so the caller cannot know how much went out
(close and reconnect if the protocol has no resumable framing).

### One operation per direction

A socket allows one operation per direction at a time: while one
context is reading from a socket — parked or still running the
call — a second `read` on the same socket from another context
raises `SocketException` (`another operation is in progress on
this socket`) rather than racing the first one for the bytes. The
usual shape is one context per connection reading and, if needed,
another writing — reads never split a stream between two readers.

### Exceptions

`SocketException` and `TimeoutException` are declared by the
package source as `const` classes deriving from `IOException`
(§8.3, §20): every net failure is an `IOException`, so
`catch (IOException e)` covers network and file errors together,
and `catch (SocketException e)` or `catch (TimeoutException e)`
narrows to the network. Both carry the usual `message` and `cause`
fields (§8.3); a platform-level failure appends the operating
system's error text to the message. `SocketException` also carries
an `errorCode` field: the operating system's error number when the
failure came from the OS (Windows: the Winsock code — `10061`
connection refused, `10048` address in use, `11001` host not
found), `0` when there is none (an argument error, an internal
condition). Branch on the number — the message text follows the
OS locale, the number does not.

### Bytes and strings

Socket payloads are `byte[]` arrays (§4.10). The `io` package's
`io.toBytes` and `io.fromBytes` (§14.5) convert between text and
raw Unicode Transformation Format (UTF-8) bytes for protocol work;
`TcpStream.write(string)` is the convenience overload that encodes
for you. `read` and `recvFrom` report the received length — convert
or forward only that many bytes, never the whole buffer.

```nlang
import net;

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    int rc = 0;
    if (srv.localPort() <= 0) { rc = 1; }   //port 0 assigned by system
    srv.close();
    return rc;
}
```
