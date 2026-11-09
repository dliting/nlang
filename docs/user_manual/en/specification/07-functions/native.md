# 7.5 Native Functions

A declaration whose flags include `native` and whose body is `;`
(§2.2, §2.4) has no body: the host supplies the implementation at
run time. A native function otherwise behaves as an ordinary
function: default arguments are filled at the call site before
dispatch (§7.2), including across module boundaries.

A free native function is keyed by `<package>.<name>`, where
`<package>` is the module path of the declaring unit (§3.9)
and `<name>` is its name. A class-member native keeps its bare
name; for an instance method the receiver is the first host
argument. On the first call the host loads the implementation for
the package; for a free native of a single-segment package the
loader searches for `nlang_<package>.dll` on the host search path.
A unit whose package name has more than one segment may not
declare native functions: a compile-time error. A native function
has no `out` parameters (§7.2).

If the host cannot find the implementation, the call raises a
host-level run-time error (§7.1). The host is responsible for
matching the declared signature: a mismatch between the declaration
and the host implementation is not a type error and yields
unspecified results (§0.4). One mismatch is specified: a native
declared to return `string` must hand back a live string handle —
the null handle, or an invalid handle that does not denote a live
string object, raises a host-level run-time error (§7.1) —
`string` has no null value (§4.3). The calling convention is §27.4.

### A native declaration

*Informative:* a `native` declaration has **no body** — the
implementation is provided by the embedding host at run time:

```nlang
native int natAdd(int a, int b);
native float natFAdd(float a, float b);
native void natPing();

int main() {
    return natAdd(20, 22);  // 42 — dispatched to the host
}
```

### How the call resolves

*Informative:* the declaration compiles to a function record that
carries only its signature (no bytecode). At the call site the VM
first consults the host-registered native table; on a miss for a
qualified key, the loader locates `nlang_<package>.dll` on the
search path and lets it register (the load triggers lazily, on the
first call); only a further miss throws. On a hit the native is
invoked directly with the caller's staged argument cells.

- The value layout — one eight-byte cell per argument, the receiver
  slot of a method, and the return cell — is §27.4 (see also
  [Stack Frame Layout](../../vm-architecture/stack-frame-layout.md)).
- Resolution failures throw at the call site — never silent garbage: a
  missing `nlang_<package>.dll` reports `cannot find native module
  'nlang_<package>.dll' for package '<package>'` (followed by the
  searched directories); a loaded module that never registers the name
  reports `native function not registered: <name>`.
- Default parameters work (filled at the call site before dispatch),
  including across module imports (defaults are serialized into the
  module file alongside the native flag).

### Restrictions

*Informative:*

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
  the host dynamic-link library (DLL) is named by the package segment
  before the first dot, so a dotted package cannot name one; compile
  error.
- Cross-module: a module importing a `.ncu` containing natives calls
  them through the same table (the native flag survives the module merge).
- Class-member `native` methods work: dispatch reaches the native through
  the normal method path, with the receiver riding in the first argument
  cell followed by the declared parameters — mirroring the bytecode
  calling convention (§27.4).
- String/struct/class argument marshalling beyond the single-slot value
  cell is not supported.

### Test hosts

*Informative:* the `ncc`, `nvm` and `ndb` binaries are test hosts —
they register a small set of test natives (`natAdd`, `natConst`,
`natFAdd`, `natPing`) keyed to specific package names; a same-named
declaration in your own project resolves through the
`nlang_<your package>.dll` lookup and never hits them. A production
embedder's host registers none of these test names; calling an
unregistered name against such a host gets
`native function not registered` at run time.
