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
only its signature (no bytecode). A free native's lookup key is
`<package>.<name>` (the package is the declaring translation unit's
module path; class-member natives keep the bare name). At the call site
the VM first consults the host-registered native table; on a miss for a
qualified key, the loader locates `nlang_<package>.dll` on the search
path and lets it register (lazy: the load triggers on the first call);
only a further miss throws. On a hit the native is invoked directly with
the caller's staged argument cells:

- application binary interface (ABI): argument `i` is the raw 4-byte cell at `args[i*4]` — little-endian
  `int32`/`float` bits or a heap index, identical to the intrinsic ABI.
  The native writes its 4-byte return value into `ret` (may be null for
  `void` natives).
- Resolution failures throw at the call site — never silent garbage: a
  missing `nlang_<package>.dll` reports `cannot find native module
  'nlang_<package>.dll' for package '<package>'` (followed by the
  searched directories); a loaded module that never registers the name
  reports `native function not registered: <name>`.
- Default parameters work (filled at the call site before dispatch),
  including across module imports (defaults are serialized into the
  module file alongside the native flag).

**Restrictions:**
- A `native` declaration **must not have a body** — compile error.
  The host owns the implementation.
- **No `out` parameters** — writeback needs a callee frame and natives
  have none. Compile error; the VM enforces the same for hand-crafted
  modules.
- Keys of free natives are **fully qualified** (`<package>.<name>`):
  same-named natives in different packages are unrelated, and one package
  plus name is a single table entry. The host registration is responsible
  for matching the declared signature; a mismatch (e.g. declaring
  `native string` over an int native) yields garbage output, not a type
  error.
- **A multi-segment package (containing `.`) may not declare natives** —
  the host dynamic-link library (DLL) is named by the package segment before the first dot, so a
  dotted package cannot name one; compile error.
- Cross-module: a module importing a `.ncu` containing natives calls
  them through the same table (the native flag survives the module merge).
- Class-member `native` methods work: dispatch reaches the native through
  the normal method path, with `this` riding at `args[0]` (the receiver's
  heap index) followed by the declared parameters — mirroring the bytecode
  calling convention. A host native therefore reads user parameter `j` at
  `args[(1+j)*4]` when bound as a method, but at `args[j*4]` when bound as
  a free function. Registering one implementation under both shapes is a
  signature mismatch (see above).
- String/struct/class argument marshalling beyond the raw 4-byte ABI is
  not supported.
- **Test host note**: the `ncc`, `nvm` and `ndb` binaries are test hosts —
  they register a small set of test natives (`natAdd`, `natConst`,
  `natFAdd`, `natPing`) keyed to specific package names; a same-named
  declaration in your own project resolves through the
  `nlang_<your package>.dll` lookup and never hits them. A production
  embedder's host registers none of these test names; calling an
  unregistered name against such a host gets
  `"native function not registered"` at runtime.

