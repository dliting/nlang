# NLang Example Index

Each example focuses on one topic and compiles and runs on its own. The
three ways to run are described in
`docs/user_manual/en/getting-started/running.md` (nide Help menu → NLang
Getting Started).

Command line (run from the repository root; installer users run from the
installation root, ncc/nvm live under `bin\`):

    ncc build examples/<name>.n -o <name>.ncu
    nvm <name>.ncu

Without PATH set up inside the installation, use `bin\ncc build ...` and
`bin\nvm <name>.ncu`.

`stdlib_io` needs one line of stdin: `nvm stdlib_io.ncu < examples/stdlib_io.stdin`.
The `net_*` examples ship a `.flags` file: the test gate runs them as
`nvm --carriers=1 <name>.ncu`, pinning the run to a single carrier.
The exit code of a successful run is in the "Exit code" column (Windows
keeps it within 0-255).

| Example | Demonstrates | Exit code |
|---|---|---|
| hello.n | smallest program | 42 |
| add.n | functions and return values | 42 |
| class_basic.n | class fields/constructor/methods/toString | 3 |
| inheritance_super.n | extends/super()/override/virtual dispatch | 4 |
| exceptions.n | throw/try/catch/finally/built-in exceptions/user subclass | 5 |
| list_tour.n | `List<T>`/add/contains/indexOf/foreach | 6 |
| dict_tour.n | `Dict<K,V>`/containsKey/keys()/foreach | 7 |
| array_initlist.n | fixed-length arrays/`[...]` initializers/length | 8 |
| interpolation_tour.n | `"${}"` interpolation/implicit toString conversion | 9 |
| cast_operator.n | `as` unboxing and reference downcast (numeric conversions go through implicit assignment) | 10 |
| const_assert.n | const locals/assert/compound assignment | 11 |
| out_params.n | `out` parameters for multiple return values | 12 |
| ndarray_tour.n | rectangular multi-dimensional arrays/comma subscript/rank()/shape()/length | 13 |
| switch_tour.n | switch/enum branches | 0 |
| enum_methods.n | enum methods and `this` | 0 |
| delegates_tour.n | function/method delegates | 7 |
| aliases_tour.n | type aliases | 0 |
| stdlib_string.n | string methods | 0 |
| stdlib_math.n | standard library `math` package | 0 |
| stdlib_io.n | io console and file content (needs stdin) | 0 |
| stdlib_fs.n | fs paths/directories/metadata | 0 |
| tensor_tour.n | `tensor` package: creation, elementwise and reduction kernels across element kinds | 0 |
| url_tour.n | `url` package: parse/inspect/resolve/query parameters/percent encoding | 0 |
| http_hello.n | `http` package: a hello service and its client in one process (buffered GET, echo POST, keep-alive reuse) | 0 |
| http_conc_bench.n | `http` package: 1000 virtual-thread clients on one shared client capped at 4 connections per host | 0 |
| http_sse.n | `http` package: server-sent events over a chunked stream, early client disconnect, producer cancellation | 0 |
| net_echo_server.n | TCP echo server: accept loop and one virtual thread per connection | 0 |
| net_echo_client.n | TCP client: connect, text exchange, `readLine` framing, graceful close | 0 |
| net_http_mini.n | mini HTTP server and client built on raw `net`: request line, Content-Length, half-close | 0 |
| net_udp_echo.n | UDP request/reply: `sendTo`/`recvFrom`, datagram semantics | 0 |
| net_ping.n | ICMP ping: round-trip latency, timeout exception handling | 0 |
| net_conc_bench.n | 1000 virtual-thread echo clients and a throughput report | 0 |
| conc_spawn_join.n | concurrency: spawn tasks and wait with `join` | 13 |
| conc_future.n | concurrency: `Future` return values | 14 |
| conc_channel_pipeline.n | concurrency: channel producer-consumer and `close` | 15 |
| conc_mutex_counter.n | concurrency: `Mutex`-protected shared counter | 16 |
| conc_semaphore.n | concurrency: `Semaphore` concurrency cap | 17 |
| conc_waitgroup.n | concurrency: `WaitGroup` over a batch of tasks | 18 |
| conc_channel_close.n | concurrency: `close` broadcast exit signal | 19 |
| conc_carriers_demo.n | concurrency: carrier count does not change results (try `nvm --carriers=1` vs `--carriers=8`) | 20 |
| conc_shared_list.n | concurrency: a shared `List` appended by two tasks, unlocked and `Mutex`-serialized | 21 |
| conc_selectany.n | concurrency: `Promise`/`Future.selectAny` multi-way waiting and the channel-converge alternative | 22 |
| hello_project/ | multi-file project (open the `.nproj` in nide) | 0 |
| libdemo/ | flat search-path library (build the library unit first; consumers `import` it and it loads at run time) | 0 |
| mnist_mlp/ | two-layer MNIST perceptron — views, broadcasting, axis reductions in batch matrix form (multi-module project; data in `data/`, gate at 0.85) | 0 |
| embed_host/ | minimal C++ embedding host (built by CMake; run `embed_host <artifact.ncu|.npkg>`) | 0 |
