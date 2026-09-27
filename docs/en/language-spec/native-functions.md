# Native Functions


A function declaration marked `native` has **no body** — the
implementation is provided by the embedding host at runtime:

```nlang
native int natAdd(int a, int b);
native float natFAdd(float a, float b);
native void natPing();

int main() {
    return natAdd(20, 22);  // 42 — dispatched to the host
}
```

**Model.** The declaration compiles to a function record that carries
only its signature (no bytecode). At the call site the VM looks the
name up in the host-registered native function table and
invokes the native directly with the caller's staged argument cells:

- ABI: argument `i` is the raw 4-byte cell at `args[i*4]` — little-endian
  `int32`/`float` bits or a heap index, identical to the intrinsic ABI.
  The native writes its 4-byte return value into `ret` (may be null for
  `void` natives).
- Calling a native the host never registered throws at the call site
  (`native function not registered: <name>`) — never silent garbage.
- Default parameters work (filled at the call site before dispatch),
  including across module imports (defaults are serialized into the
  module file alongside the native flag).

**Restrictions:**
- A `native` declaration **must not have a body** — compile error.
  The host owns the implementation.
- **No `out` parameters** — writeback needs a callee frame and natives
  have none. Compile error; the VM enforces the same for hand-crafted
  modules.
- The table is keyed by **declaration name only**. The host registration
  is responsible for matching the declared signature; a mismatch (e.g.
  declaring `native string` over an int native) yields garbage output,
  not a type error. Two modules declaring the same native name share one
  table entry.
- Cross-module: a module importing a `.nmod` containing natives calls
  them through the same table (the native flag survives the module merge).
- Class-member `native` methods work: dispatch reaches the native through
  the normal method path, with `this` riding at `args[0]` (the receiver's
  heap index) followed by the declared parameters — mirroring the bytecode
  calling convention. A host native therefore reads user parameter `j` at
  `args[(1+j)*4]` when bound as a method, but at `args[j*4]` when bound as
  a free function. Registering one implementation under both shapes is a
  signature mismatch (see above).
- String/struct/class argument marshalling beyond the raw 4-byte ABI is
  not supported yet.
- **Test host note**: the `ncc` and `nvm` binaries are test hosts — they
  always register `natAdd`, `natConst`, `natFAdd`, and `natPing` so the
  test suite can exercise the binding path. A production embedder's host
  does not register these test names; scripts calling those names against
  such a host will get `"native function not registered"` at runtime.
