# 21.2 The Transmission Control Protocol (TCP) Classes

TCP (Transmission Control Protocol) is the connection-oriented,
ordered, reliable byte stream. `TcpListener` accepts incoming
connections; `TcpStream` is one established connection. The parking
and timeout convention and the one-operation-per-direction rule are
§21.1: every blocking method parks the calling context, raises
`TimeoutException` at its deadline, and raises `SocketException` on
a closed or failed socket.

## TcpListener

| Member | Shape | Semantics |
|---|---|---|
| `TcpListener.bind(host, port, backlog = 512)` | static → `TcpListener` | Binds and listens on `host:port`; `backlog` is the accept-queue depth handed to the operating system (raise it before a connect storm — a full queue refuses further connects; the kernel clamps the value, and Windows loopback holds only about 200 pending connections no matter how large the request, so a client seeing a refused connect under a storm retries). Raises `SocketException` when the address is in use or unavailable |
| `TcpListener.bind(port)` | static → `TcpListener` | Shorthand for `bind("0.0.0.0", port)` — all interfaces |
| `accept(timeoutMs = 0)` | → `TcpStream` | Waits for the next connection and returns it as its own `TcpStream`; parks until one arrives or the deadline passes |
| `localAddress()` | → `string` | The bound address |
| `localPort()` | → `int` | The bound port — the system-assigned port when `bind` used port `0` |
| `setRecvBufferSize(bytes)` | — | Requests the receive buffer size for connections accepted from this listener; set it before those connections are established to take effect |
| `close()` | — | Stops listening; idempotent. Connections already accepted are unaffected |

`accept`'s `TimeoutException` is a polling tool, not an error: a
server loop that also watches a shutdown flag catches it and loops
again — the shipped `examples/net_echo_server.n` does exactly this,
and closing the listener ends the loop through `SocketException`.

## TcpStream

| Member | Shape | Semantics |
|---|---|---|
| `TcpStream.connect(host, port, timeoutMs = 0)` | static → `TcpStream` | Resolves `host` and establishes a connection, trying every resolved address in turn; the resolution and the wait both park the caller (§21.1); raises `TimeoutException` when the deadline passes before any address answers, `SocketException` when resolution fails or every connection attempt is refused |
| `read(buf, timeoutMs = 0)` | → `int` | Fills up to `buf.length` bytes of a non-empty `buf` and returns the count; `0` means end of stream — the peer closed its write side; an empty `buf` raises `SocketException`, because `0` is reserved for that end-of-stream signal. Bytes arrive in order; one `read` may return fewer bytes than asked for |
| `write(data, count = -1, timeoutMs = 0)` | → `int` | Sends `count` bytes of `data` — all of it when `count` is `-1`; parks until the bytes are handed to the stack. `timeoutMs` bounds the whole write-all: if the peer stops reading and the deadline passes, raises `TimeoutException` with part of the payload already sent |
| `write(s, timeoutMs = 0)` | → `int` | Text shorthand: encodes `s` as Unicode Transformation Format (UTF-8) bytes and sends them, with the same timeout behavior as the byte overload |
| `readLine(timeoutMs = 0)` | → `string` | Reads up to and past the next `\n`, strips it (and a preceding `\r`), and returns the line; the partial line when the stream ends mid-line, and the empty string when it ends with nothing read — an empty line and end of input look the same here (`string` has no `null` value, §4.3), so `read` returning `0` is the end-of-input signal |
| `readExactly(count, timeoutMs = 0)` | → `byte[]` | Reads exactly `count` bytes into a fresh array; raises `SocketException` when the stream ends early — the right tool for a `Content-Length`-framed body |
| `shutdownWrite()` | — | Half-close: sends the end-of-stream marker to the peer while the read side stays open (below) |
| `close()` | — | Closes the connection; idempotent. A parked reader on the peer sees end of stream |
| `isClosed()` | → `bool` | Whether this handle has been closed |
| `peerAddress()` / `peerPort()` | → `string` / `int` | The remote end |
| `localAddress()` / `localPort()` | → `string` / `int` | The local end |
| `setNoDelay(on)` | — | Disables Nagle batching, for latency-sensitive protocols |
| `setKeepAlive(on)` | — | Enables TCP keep-alive probes |
| `setSendBufferSize(bytes)` / `setRecvBufferSize(bytes)` | — | Requests the socket buffer sizes |

## End of stream and half-close

`read` returning `0` is the ordered end of the byte stream: the
peer called `shutdownWrite` or `close`. Either side can close only
its write side with `shutdownWrite` — the classic request/response
shape, where the client signals "request complete" while still
reading the reply.

The echo round trip in one process — bind, connect, accept, one
line each way, half-close, self-exit:

```nlang
import io;
import thread;
import net;

class Echo {
    public TcpStream conn;
    public void run() {
        string line = conn.readLine(5000);
        conn.write("echo:" + line + "\n");
        conn.close();
    }
}

int main() {
    TcpListener srv = TcpListener.bind("127.0.0.1", 0);
    Echo e = Echo();
    TcpStream cli = TcpStream.connect("127.0.0.1", srv.localPort(), 5000);
    e.conn = srv.accept(5000);
    Thread t = thread.spawn(e.run);

    cli.write("morning\n");
    cli.shutdownWrite();              //half-close: done writing
    string reply = cli.readLine(5000);
    io.print("server says: " + reply + "\n");
    cli.close();
    thread.join(t);
    srv.close();
    return 0;
}
```

The same shape at server scale — an accept loop spawning one
virtual thread per connection, a thousand of them at once — is the
shipped `examples/net_echo_server.n` and `examples/net_conc_bench.n`;
`examples/net_http_mini.n` layers line framing and
`Content-Length` bodies on top.
