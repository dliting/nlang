/*---
    multi_gpu_impl_plan.md — the multi-GPU device line v2
    implementation plan (detailed design over the spec
    multi_gpu_design.md, three-party reviewed to APPROVE). One
    section per milestone M-G1..M-G4: exact signature/ABI changes,
    file-level touch lists, TDD order, gates. The plan does not
    restate spec rulings; it cites their sections.
---*/

# Multi-GPU implementation plan

Conventions: TDD per milestone (failing test first, AGENTS.md);
after `src/vm`/`src/compiler`/`include/` changes rebuild the full
config (no named-target lists); Windows four-leg gate for shared
commits, box legs for cuda behavior; new e2e entries add
`tests/e2e/manifest.txt` rows (`test_name expected_exit_code`), and
new device tests also add `tools/e2e_cuda_double_run.txt` lines
(the runner validates that list against the manifest); new ctest
entries register their timing rows through the reference-timing
suite now carried on this branch (port at 19a79583): run the entry
once for junit, merge with `tools/gate_time_check.py record` — the
suite's guard wiring into `gate_tier.sh` lands with the branch's
keyed gate-tree rework, not this line; every user-visible
entry lands VERSION + manual (en/zh) + CHANGELOG in the same
milestone (spec §9), and the stdlib symbol pins in
`test_library_index.cpp` / `test_symbol_index.cpp` move with it.

## 1. Shared contracts (what changes once, at the boundaries)

Spec §4/§5 in code terms — the exact surface deltas M-G1 introduces:

**NativeHost ABI v16** (`include/nlang/vm/NativeHost.h`):

- `newArrayResident(self, elemKind, rank, dims, device)` — the
  device argument lands before the payload exists; the v16 block
  comment records it (the entry sits mid-table; the load-time
  version rejection makes in-place widening safe, spec §5).
- `TensorDeviceHooks`: `alloc / zero / free / copyH2D / copyD2H`
  gain `int32_t device` as the first parameter; `sync` unchanged
  (VM-uncalled, spec §5). Doc comments at the `NativeArrayView`
  record: `reserved[0]` class unchanged, `reserved[1]` = device id,
  meaningful only when `reserved[0] == 1`.
- ABI-version constant bumped once (v15 → v16).

**Device ABI** (`src/native/tensor_cuda/backend_descriptor.h`).
The load-time version check is new M-G1 work, not standing
discipline (round-1 finding): the plugin exports
`nlang_device_abi_version` — `cuda_backend.cpp:810` returns `1u` —
and the symbol name is defined (`backend_descriptor.h:33`), but no
loader code ever calls it. M-G1 adds the expected-version constant
`NLANG_DEVICE_ABI_VERSION` to `backend_descriptor.h` (the header
both sides include — the single authority, mirroring the host
pattern), resolves the probe in `plugin_loader.cpp` through the
existing resolve helper (`:42`/`:54`), and rejects a mismatch at
load, no fallback (the host-side analog is `NativeHost.h:423-426`).
Bumps: `1u→2u` at M-G1 (the `streamCreate(int)` signature change),
`2u→3u` at M-G3 (the `copyD2D` entry lands at the struct's tail —
an old plugin's descriptor has no such field, so the version bump
is the guard):

- `streamCreate(int device)` (was `()`);
  `streamDestroy/Push/Join(int handle)` unchanged — the handle
  encodes the device (below); `streamPop()` unchanged — the
  plugin's push stack carries the device per entry.
- New entry `TensorDeviceStatus (*copyD2D)(int32_t dstDevice,
  void* dst, int32_t srcDevice, const void* src, size_t bytes)` —
  the registry-side cross-device transfer (M-G3); null = the
  backend cannot cross, the registry raises by name. The entry
  implements the whole ordering contract (spec §6): ordered after
  the source device's current stream's work, an event handoff into
  the destination device's current stream, peer or pinned-host
  staging per the matrix.
- Device-ABI version bumped; the registry's load check rejects a
  mismatch (same discipline as the host ABI).

**Stream handle encoding** (documented in backend_descriptor.h):
`handle = device * kNativeMaxStreams + slot`, `kNativeMaxStreams =
16` (the existing pool depth, cuda_kernels.cu — magic number to a
shared constant in backend_descriptor.h so plugin and registry
decode identically). **Slot 0 of every device's pool is reserved
as that device's implicit main stream and never handed out** (spec
§5): handle 0 decodes to (device 0, main stream) on every backend,
matching the cpu/mirror no-op sentinel; cuda's first chunk handle
becomes 1 (the round-3 review verified every corpus handle is
opaque).

**DeviceBackendState** (`backend_registry.h:53-65`):
`+ int deviceCount` (cpu 0, mirror 1, cuda = the probe count the
activation currently checks-and-drops, backend_registry.cpp:138-157)
and `+ copyD2D` (from the descriptor).

**VM device vector** (`VmExecutor.h`, beside `m_arrResidency`):
`ChunkedArray<uint8_t> m_arrDevice` — the sibling's container
(`VmExecutor.h:2644`), whose `Emplace`/block-growth semantics the
five sites use; same lifecycle, five sites:
the resize/sentinel pair (VmExecutor.cpp:158-162), the block-growth
co-append (VmExecutorAlloc.cpp:41), the free-list reuse re-assert
(VmExecutorAlloc.cpp:180 → 0), the sweep release (read the entry
BEFORE clearing, hand it to `hooks.free(device, …)`; clear to 0,
VmExecutorGCSweep.cpp:62-70), and the device mint (write the
`newArrayResident` argument's device). Owner records only — the
view arm reads the OWNER's entry at the same owner-indirection
sites residency resolution already uses (spec §4).

**View fill** (`VmExecutorNativeArray.cpp:42-111`): with
residency 1, `reserved[1] =` the (owner's) device entry; host arm
stays 0. `NativeHostHelpers.h:71-72` (host-only fillers) stay 0.

**Copy wrappers** (`backend_registry.h:91-94`, a signature delta
the first draft omitted — round-1 finding): `CopyHostToDevice` and
`CopyDeviceToHost` gain `int32_t device`; the native call sites
take it from the device-side view's `reserved[1]`
(`tensor_random.cpp:121`, `StageOperandToHost` at
backend_registry.cpp:326/334) or from the transfer's new target
argument (`TransferToDevice` :254, M-G3).

**Test skip contract** (user ruling 2026-11-26: device-dependent
tests must RUN everywhere — no GPU and one GPU included — and what
cannot be tested must REPORT itself; "all green" is not the
requirement, "runs + visible, reasoned skip" is). One rule, three
layers:

- The test program checks its own capability at runtime, against
  the authority for exactly what it asserts: `tensor.deviceCount()`
  through the active backend (cpu 0 / mirror 1 / cuda N). A
  harness-side GPU oracle is rejected deliberately: it would itself
  run nvm and must pick the same backend as the run — the
  program-side check IS that authority; `env_gates.txt` keeps its
  stated scope (true out-of-band OS capabilities: icmp/dns).
- Insufficient capability: print one reasoned line on stderr —
  `skip: deviceCount() = 1 < 2`, carrying the observed value (the
  same channel the corpus's runtime diagnostics use) — and return
  the reserved skip exit code `90` (`E2E_SKIP_EXIT = 90` in the
  runner, mirrored at the program's return site; distinct from the
  explicit-cuda fail-fast `3` and every expectation the manifests
  use today). The exit code is THE signal — classification never
  scrapes stdout.
- e2e runner (`run_e2e_tests.py`): an exit of `E2E_SKIP_EXIT`
  classifies as a skip — printed `SKIP {name} (reported: <reason
  line>)` and counted in the existing skipped tally (widening its
  `(env-gated)` label to cover reported skips; the runner already
  promises "counted, never silently green"). A reason line is
  MANDATORY: skip code without one = FAIL, so the reporting duty is
  enforced, not encouraged. Manifest expectations stay `0` — no
  opt-in marker: a stray `90` from a test that cannot legitimately
  skip still surfaces (with its reason, or as an unreported FAIL).
- ctest (`mnist_ddp_run`, M-G4): the same sentinel through the
  `SKIP_RETURN_CODE` property (ctest 3.8+, the project floor is
  3.16) — the entry lands in ctest's native Skipped state, visible
  in the summary, never failing the suite.
- Gate legs stay green across skips: the coverage invariant stays
  per-leg (`tools/tier_coverage_check.py` — the net is held by the
  legs that DO have the capability: the box cuda runs), and
  visibility rides the SKIP lines and counts. `gate_cuda_tree.sh`
  keeps its probe-level `SKIP (reason)` for the no-plugin/no-driver/
  no-device case — a whole-leg skip of the same shape.
- Never assert an exact device count (`>= 1`/`>= 2` guards only);
  no new e2e/ctest entry may require a device to exist except
  through this contract.

## 2. M-G1 — ABI + boundaries (device-invisible)

TDD order; each step's test lands before its implementation:

1. Unit (tests/test_native_tensor): view `reserved[1]` round-trip
   through `getArrayView` (mirror-backed: hooks see the device arg;
   host views stay 0); handle encode/decode pure-function test
   against `kNativeMaxStreams`; `AssertSameDevice` message names
   `cuda:0` / `cuda:1` / host (extend the mixed-residency message
   tests).
2. VM: the device vector + five fill/clear sites + the view-fill
   arm; `DeviceLoadElement/DeviceStoreElement/DeviceStageToHost/
   DeviceStageFromHost` and the format/debug/host-bridge staging
   arms pass `m_arrDevice[heapIdx]` down; `AllocDeviceArrayOnHeap`
   takes the device from `NativeNewArrayResident`.
3. ABI widen at `TensorDeviceHooks`: the mirror hook set
   (backend_registry.cpp:28-65) and the cuda plugin's hook
   implementations gain the parameter; the cuda set gets the
   context-pin guard — a small RAII `SetDeviceForCall` helper
   calling `cudaSetDevice(device)` (the guide's discipline,
   replacing the single boot-time `cudaSetDevice(0)` at
   cuda_kernels.cu:190 as the ONLY context assumption left).
   Same step wires the load-time device-ABI probe (§1):
   `plugin_loader.cpp` resolves the exported version symbol,
   compares against the new `NLANG_DEVICE_ABI_VERSION` constant,
   rejects a mismatch; the cuda export bumps to `2u`. The compare
   lands as a pure function so a unit test covers the reject path
   without a stub plugin.
4. Registry: `AssertSameResidency` → `AssertSameDevice` (rename at
   the 6 dispatcher call sites — `tensor_activation.cpp:40`,
   `tensor_reduce.cpp:130`, `tensor_gather.cpp:94`,
   `tensor_binary.cpp:55`, `tensor_matrix.cpp:71`/`:130` — plus
   the declaration/definition pair `backend_registry.h:206` /
   `backend_registry.cpp:426`; message per step 1); staging choke
   point clears `staged.reserved[1]` (backend_registry.cpp:316-319);
   `MintOutputLike` passes `like.reserved[1]` on the device path;
   `MintArray/MintArray1` pass `CurrentScopeDevice()` (new helper,
   step 5) and `newArrayResident(…, device)`.
5. `backend_stream.cpp`: a fixed push stack (depth cap constant,
   the existing depth counter becomes the stack size — the PRNG
   `StreamPushDepth()` contract unchanged);
   `CurrentScopeDevice()` = decode(stack top) or 0;
   `BackendStreamCreate(int device)` routes to the widened
   descriptor entry; Pop keeps raising on imbalance as today.
6. Plugin (cuda): `CudaStreamCreate(device)` gates the device against
   the probed census (the single `CudaDeviceAccepted` authority —
   the payload hooks gate there too), reserves slot 0, encodes
   handles; push/join/destroy decode (a stale or foreign handle
   takes the sentinel-shaped path: Push still saves its frame
   without swapping — the plugin stack stays in lockstep with the
   registry's — and destroy/join no-op; the descriptor's stream
   entries return void, there is no raise channel — amendment at
   M-G1 landing, the first draft's "invariant raise" has nowhere
   to raise); the push stack entries carry (device, saved current)
   so interleaved cross-device Push/Pop restore correctly;
   per-device `g_currentStream[d]` (still one device deep in M-G1,
   the table shape lands now so M-G2 only fills it).
7. Manual: one line moves — the quoted mixed-device diagnostic in
   `library/19-tensor.md` §19.7 gains the device id
   ("cuda vs host" → "cuda:0 vs host") with the message change, so
   both locales update in this batch (amendment at M-G1 landing: the
   plan first said "nothing"; the step-5 rename makes M-G1 a
   user-visible diagnostic change, and the five-category check
   counts it). Everything else stays header-comment authority: the
   first draft's `formats/30-abi.md` does not exist on this branch
   — round-1 finding — no user-manual chapter documents the
   device-plugin contract, and its authority is the
   `backend_descriptor.h` header comments (the handle encoding,
   `reserved[1]`, v16 entries land there). `formats/28-abi.md` is
   the native-module ABI — a different contract this line does not
   touch (its "version 12" stamp is pre-existing drift on this
   branch).
8. Full rebuild (both configs), Windows four legs, box cuda build +
   the cuda double-run subset (`tools/e2e_cuda_double_run.txt`,
   205 rows at branch state) + single-GPU mnist. Expected:
   everything green with deviceCount() never consulted — M-G1
   introduces no user-visible device choice.

Exit: corpus bit-identical behavior; every boundary carries the id;
the only files M-G2+ will rework are the plugin internals.

Landing rulings recorded at M-G1 (review round-1, 2026-11-26):
VERSION stays 0.14.10 — the step-7 diagnostic-wording change ships
with the line's next checkpoint (M-G2's 0.14.11), whose CHANGELOG
entry covers it (the milestone model, spec §9 — not drift); the
negative-handle decode contract (C++ truncating division: −1 →
(0, −1)) lands as `backend_descriptor.h`'s contract comment instead
of a unit test — every consumer gates `handle <= 0` before
decoding, so pinning the truncating value would freeze a detail no
reachable path observes.

Review round-2 rulings (2026-11-26): the `CudaStreamPush` pre-init
frame-capture fix (N1) carries no dedicated unit test — it is
value-preserving and plugin-internal (no host-observable seam before
or after), so the cuda double-run is its verification, following the
negative-handle precedent's reasoning; the step-1 device round-trip
gap (Low 5) and the guard-config coverage gap (N4) landed as the
`test_native_array_abi` resident-mint test and the guard selftest's
config-validation cases.

Cross-platform duty (user ruling, 2026-11-26): device-dependent
tests must run on machines without a GPU and on single-GPU
machines, and untested cases must report themselves — the binding
formulation lives as the skip contract in §1. M-G1's own surface
needed no gating to meet it: the default cpu backend runs the whole
device/stream family on the 0-GPU Windows trees, and
`gate_cuda_tree.sh` already reports `SKIP (reason)` when the probe
finds no plugin/driver/device.

Post-M-G1 fix (2026-11-26): ndb displayed a view over a
device-resident owner unstaged — pointer fragments as elements,
reads past the record end — because the debugger's
`FormatDebugViewArray` lacked the staging arm the print path's
`FormatViewArray` already had; the debugger's record arm carried
the staging open-coded, drifted from the shared helper. Both
debugger arms now route through `StageDeviceArrayForFormat`, the
print path's own primitive (the display twin, not a new device-aware
inspection surface — spec §10.3's "ndb stays out" still holds).
VERSION 0.14.11 opened early with the fix: the checkpoint was
already reserved for this line and M-G2's entry folds into the same
section. Coverage: the `dbgm_device_record`/`dbgm_device_view` e2e
pair pins both arms; the `.env` mirror pin keeps them running on
every machine, no GPU required.

## 3. M-G2 — per-device pools + deviceCount()

Plugin (cuda_kernels.cu) — the singletons join a
`CudaDeviceState[MAX_CUDA_DEVICES]` table (cap constant, 16):

- per-device: mempool (today's `cudaDeviceGetDefaultMemPool(&g_
  memPool, 0)` becomes `[d]`), stream pool with slot-0 main stream,
  `g_currentStream[d]` (M-G1), error-flag buffer, staging
  stream/event pair, sticky fault register, the peer matrix row
  (queried + enabled at first device use, spec §6 — the ordered
  pair loop moves here from the never-created contexts of probe
  time).
- lazy init becomes per-device: first call pinning device d runs
  `cudaFree(0)` (context) + the per-device buffers + the peer-row
  enablement; `CudaLazyInit` keeps its one-time process parts
  (device enumeration).

Registry/natives:

- activation stores `deviceCount` (cuda: probe; mirror activation
  path sets 1; cpu stays field-default 0).
- `tensor.deviceCount()` native (`TensorDeviceCount`) +
  `native int deviceCount();` in stdlib/tensor.n.
- `native int streamCreate(int device);` overload;
  `BackendStreamCreate(device)` guards: cpu accepts only 0,
  mirror/cuda require `0 <= device < deviceCount`.

Tests (TDD first):

- e2e `op_device_count.n`: asserts `deviceCount()` matches
  `backend()` (cpu 0 / mirror 1 / cuda >= 1 — the box has 2, never
  assert an exact cuda count).
- e2e `op_stream_device_guard.n`: cpu `streamCreate(1)` raises,
  mirror `streamCreate(1)` raises + `streamCreate(0)` returns 0,
  cuda `streamCreate(deviceCount())` raises. All three run on every
  leg (the guard matrix is backend-conditional inside the test,
  the corpus already runs per-backend legs).
- unit: the peer-row query code path on a single-GPU machine
  (matrix all-false, no enable calls) compiles/runs; the encode
  test from M-G1 extended to a foreign-device handle decode.

Landing duties: VERSION 0.14.11; manual en/zh
`library/19-tensor.md` §19.7 (deviceCount, streamCreate(device));
CHANGELOG; `tests/e2e/manifest.txt` rows +
`tools/e2e_cuda_double_run.txt` lines for the two new tests;
pins 500→502; `tools/manual_sync_check.py` pass + `SYNC_BASELINE`
advance. Box: the double-run subset with both entries (207 rows),
`op_device_count` under cuda reports 2, mnist single-GPU
unchanged.

M-G2 landing (2026-11-26), rulings and deviations:

- The census guard lives at the registry
  (`BackendStreamCreate(host, device)`): device 0 always names the
  main stream, the first refused id is `max(1, deviceCount)`, and a
  foreign selector raises with both the selector and the census
  spelled ("tensor.streamCreate: device <d> beyond the device
  count <c>"). The overload dispatches on `argc`; the zero-argument
  spelling keeps riding the scope unchanged.
- The sticky fault register split into one row per device (a fault
  on one device must not blackhole the others), and every hook
  gates the selector before draining — the order reversal keeps a
  refused foreign call from consuming another device's parked
  record. `HookSync` consults row 0 while every enqueue still rides
  device 0 (M-G3 routes the enqueues).
- The plan's "error-flag buffer" landed as a row field of the
  kernel cell-error flags (`g_error` was that buffer, not a second
  one): per-device flags, allocated and cleared at row init. The
  row-0 reference aliases (`g_stagingStream`/`g_stagingDone`/
  `g_error`) keep the pre-M-G2 transfer and flag call sites
  untouched until M-G3's operand routing replaces them.
- The per-device default-mempool attribute set replaced
  `InitMemPoolOnce` (stream-ordered allocation needs no pool
  handle; the row's one-time `UINT64_MAX` release threshold is what
  makes the pool behave). `CudaDeviceAccepted` folding a failed row
  init into the same "unknown device" fault is deliberate: the
  user-visible effect of an unserviceable device is the fault.
- Scheduling ruling (user, "尽量加快中间阶段的测试"): the ndb fix's
  full T1 merges into M-G2's single closeout T1 instead of two
  back-to-back full runs; the local 207-row cuda double-run drops
  (the box double-run is the line's cuda regression — a single-4090
  laptop adds no coverage the box will not repeat); ASan legs defer
  to the same closeout. The intermediate check is the T0 shape: the
  touched unit families (`native_tensor_scalar`, `library_index_tests`,
  `langservice_tests`) + the device/stream e2e cluster — all green
  on the rebuilt Debug tree, plus the new-surface smoke on the
  bench tree's cpu/mirror/real-cuda legs.
- Gate-ops finding: the e2e runner's stdlib freshness guard rejects
  an e2e leg (rc=2, "stale-package results are meaningless") when
  `stdlib/*.n` is newer than the tree's `stdlib.npkg` — so during a
  prebuilt-tree gate run, only edits outside `stdlib/` are safe;
  a `stdlib/` edit voids that batch (one T1 batch was stopped this
  way on 2026-11-26, zero cases actually failed). Stopping a batch
  must kill the whole process tree: the runner's python survives
  stopping the wrapper script and keeps compiling the corpus with
  the tree's `ncc`, locking the tree binaries (a later rebuild died
  on LNK1168 until the orphan was found and killed).
- Review loop (third party, two rounds, 2026-11-26): round 1 found
  two Medium and five Low — the row-init retry (overwrote handles/
  allocations on partial failure) and the docs promising full scope
  routing while M-G2 routes only the creations (kernels/transfers
  still land on device 0 until M-G3). Both fixed: the retry now
  RESUMES (each step builds only what is still null), CUDA's
  last-error residue is cleared on every entry/exit path of the row
  init, and the manual (en/zh), the stdlib declaration comment and
  the CHANGELOG (en/zh) state the half-routing and the always-valid
  device 0 by name. Round 2: all nine findings RESOLVED, one new
  Low (the failure returns of the row init still left a residue
  flag that another device's ready row could mis-attribute) —
  folded. The push-device-1 e2e defers to M-G3: its skip depends
  on the runner's skip contract (§4, M-G3 is its first consumer),
  and the design pins M-G2's acceptance at "corpus green on device
  0", so the deferral hides no M-G2 regression.

## 4. M-G3 — cross-device transfers (toDevice(a, device))

- stdlib/tensor.n: four `toDevice(Array<T>, int)` overloads; native
  entries mirror the existing per-kind toDevice registration.
- Registry `TransferToDeviceD(host, ret, args)` (the four kinds
  share it; kind from the operand): validate `device` against
  `deviceCount` (raise "tensor.toDevice: device index out of
  range"); operand host → the existing H2D path with the target
  device (MintArray(device) + `CopyHostToDevice` via the widened
  hooks); operand device d0 == device → return the handle (the
  standing same-residency contract); d0 != device → operand
  materialized by SOURCE residency (`MaterializeByResidency` as
  today), fresh mint on the target, one `copyD2D` call; cpu
  backends: every arity-2 call raises (empty range, spec §3).
- Plugin `copyD2D`: peer path — `cudaMemcpyPeerAsync` enqueued on
  the SOURCE device's current stream, event recorded there, the
  DESTINATION's current stream `cudaStreamWaitEvent`s (spec §6);
  staging path — pinned host buffer (grown-doubling on first use,
  per process), D2H on source, H2D on destination, same event
  handoffs; both paths return the status struct the registry
  converts with the standing device-status mapping.
- `toHost` unchanged (class transfer; device arg flows via hooks).

Tests:

- e2e `op_tensor_cross_device.n` (the `op_tensor_*` family the
  manifest keys on, `manifest.txt:2370+`; on
  `deviceCount() < 2` prints the reasoned skip line and returns the
  skip sentinel — §1's skip contract): create a on the default
  device, `b =
  tensor.toDevice(a, 1)`, `add(b, b)` equals the host-computed
  result after `toHost`; `add(a, b)` raises naming `cuda:0` and
  `cuda:1`; run-twice byte-identical output (determinism).
- e2e guard legs already run everywhere via mirror=1 (M-G2 file
  covers the raise side; M-G3 adds `toDevice(a, 1)` on cpu/mirror
  raises to the guard test).
- unit: `copyD2D` staging-vs-peer branch selection from the matrix
  (on the fleet: always staging; the peer branch's compile +
  query-only coverage is spec §6's stated unreachable branch).

Landing duties: VERSION 0.14.12; the device-ABI bump to `3u` with
the `copyD2D` entry (§1); the runner side of the skip contract
(`E2E_SKIP_EXIT` classification + mandatory-reason enforcement +
tally widening in `run_e2e_tests.py`, pinned with pure-injection
cases in `test_runner_env_gate.py` — M-G3 is the first consumer);
manual `19-tensor.md` §19.7 toDevice overload (en/zh) + the
known-limitations line review; CHANGELOG; manifest row +
double-run line for `op_tensor_cross_device.n` (subset 208 rows);
pins 502→506; SYNC_BASELINE. Box: cross-device e2e green both
runs; Windows/single-GPU legs: the reported skips (SKIP lines +
tally) and guard paths.

M-G3 landing (2026-11-26), rulings and deviations:

- The one-argument `toDevice` follows the stream scope (it mints on
  `CurrentScopeDevice()`), closing the half-routing M-G2 recorded:
  computation follows the operands everywhere — every op file selects
  the ops table through `OpsForResidency(host, view)` from the view's
  residency, never from device 0. The manual (en/zh), the stdlib
  declaration comments and the CHANGELOG (en/zh) state full routing;
  the "kernels and transfers still land on device 0" sentences and the
  concat-axis known-limitations line are deleted.
- `toDevice(a, device)` is a registry class-2 entry with the census
  guard (the streamCreate twin): the raise names selector and census
  ("tensor.toDevice: device <d> beyond the device count <c>"), and
  every cpu call is refused — cpu's census is empty, unlike
  `streamCreate`, where device 0 names the implicit main stream. The
  same-residency contract stays device-scoped (an operand already on
  the named device returns as-is); a foreign operand materializes by
  SOURCE residency (dense on d0 — never a stride walk through
  `copyD2D`), mints on the target, crosses once.
- Device ABI `3u`: `copyD2D` lands at the descriptor's tail (an old
  plugin lacks the field — the exported version is the layout guard).
  The ordering contract (spec §6) is implemented inside the plugin:
  ordered after the source's current stream, event handoff into the
  destination's current stream, peer vs pinned-staging per the
  capability matrix. `HookCopyD2D` gates BOTH ids — a sticky fault on
  either row involves this copy's hardware and drains first — and a
  new fault attributes to the DESTINATION row, because the caller's
  handle and the folded status name the device the caller holds.
- `SetDeviceForCall` is the only spelling that changes the thread's
  device or the selected per-device row, and one pin makes an entry
  body device-correct (stream, staging, error-flag reads follow the
  pin): launchers pin from the primary `LaunchView`'s device, hooks
  from their device argument, and the row-0 aliases are gone from
  entry bodies. `HookSync` keeps the pre-M-G3 whole-device spelling
  pinned to device 0 explicitly — under operand routing the VM never
  routes there, each compute op observes its own row.
- cuda table audit: all 104 `TensorOps` slots are populated (the §R6
  "null-slot stub" comment was stale — unimplemented slots carry named
  raises, never nulls; comment corrected) and the per-family element-
  kind literals were verified, catching one real defect: the f64/i64
  concat slots aliased the f32/i32 lambda with the 32-bit kind code
  hardcoded, so the 64-bit joins copied half the bytes of each cell
  (wrong values, zeros at the tail — creation-era, surfaced by the
  merged dev corpus, not an M-G3 regression). Ruling: a kind-shared
  slot body carries the kind on the operand view (`a.elemKind`, the
  matmul/assignInto convention), never a hardcoded literal.
- Device `concat` generalizes to every axis: the join kernel folds
  each destination cell through its join-axis coordinate (the dense
  axis-0 two-span memcpy stays as the fast path). The old
  `cudaErrorNotSupported` gate raised where the host path had results
  and existed only while the parity fixture had not landed; the
  fixture landed and the throw-shaped one was renamed —
  `op_tensor_concat_axis_throw.n` → `op_tensor_concat_axis_inner.n`
  (manifest row follows) — asserting device/host agreement on axis 1.
- Zero-cell launch defect class: a zero-block grid is an invalid CUDA
  configuration; 17 launchers faulted on empty inputs/outputs. Every
  grid source (element count, line count, index count, matmul's
  row×column product) is audited and guarded — launch nothing, the
  already-correct empty result stands. The empty-raise symmetry is
  untouched: sort/arange/gather/takeAlong/scatterAdd/max raise
  "empty array" host-side on both backends by design; the guards cover
  only the launch paths that legitimately carry empties
  (full/compare/clamp/reshape/concat/matmul).
- Skip contract (runner side, M-G3 is its first consumer): an
  unserviceable fixture exits `E2E_SKIP_EXIT` (90) naming its reason
  on stderr; both runner verdict passes classify `SKIP <name>
  (<reason>)`, the tally widens, and a sentinel without a reason is a
  FAIL. The classification is pinned with pure-injection cases in
  `test_runner_env_gate.py` (5 new, 14 total green) and was verified
  live on all three arms (cuda 1<2, cpu 0<2, reasonless probe FAIL).
  `op_tensor_cross_device.n` registered with expectation 0 — no
  timing rows (e2e configs ride the `default` range rows); the
  double-run roster (`tools/e2e_cuda_double_run.txt`) carries the
  renamed row and the new one. The bench cuda leg runs the roster
  green on this machine. Counting note: the runner issues verdicts
  per MANIFEST entry and scans BOTH `manifest.txt` and
  `examples_manifest.txt`, so the roster's line count and the
  verdict count are not interchangeable — the six fixtures
  double-listed in `manifest.txt` are double-listed in the roster
  too (they cancel), and this tree's one extra verdict is
  `class_basic`, listed in both manifests (round-2/round-3 review;
  verified against the log: 213 verdict lines over 212 roster
  lines). The single skip is
  `SKIP op_tensor_cross_device (skip: deviceCount() = 1 < 2)`, the
  exact reported-skip shape the user's cross-platform ruling asks
  for. The mixed-raise and scope-gate message texts were verified
  word-for-word against the fixture's expected strings
  (`AssertSameDevice`, the `OpsForResidency` scope gate).
- Scheduling: no full T1 at this intermediate milestone (user ruling
  "中间阶段的测试不要太多"): the local check was the touched 65-row
  cuda/cpu subset double-run plus targeted probes, all green. The
  corpus double-runs happen on the acceptance box (2×4090 — the
  single-GPU laptop adds no cuda coverage the box will not repeat),
  and the full T1/T2 shape runs once at closeout on the committed
  tree. The T1 batch started before today's cuda-table fixes closed
  informational, every red diagnosed: `source_size_guard` in
  debug-unit and ide-ctest (both since refactored away), both e2e
  legs rc=2 from the runner's stdlib-freshness guard (the accepted
  in-batch stdlib edit — zero cases actually ran), and asan-unit's
  single failure `nd_resolve_stress_tests (Timeout)` — an
  environment-slow DNS stress case under ASan, watch item for the
  closeout run, not an M-G3 surface.
- Pin-lag finding (the stale-binary mask, again): the post-milestone
  full Debug rebuild — the first to compile the whole uncommitted
  M-G3 tree — surfaced two pins that lagged work already in the
  tree: the `TestStreamHandle` device-ABI pin (still 2 while the
  header had moved to 3 — updated, with the v2-refusal probe arm
  replacing the old "future version" arm) and the stdlib symbol
  pins (the planned 502→506). Lesson for every future ABI bump:
  the version pin in `TestStreamHandle.cpp` moves in the same edit
  as `NLANG_DEVICE_ABI_VERSION` — the load-time probe and the pin
  are the two eyes of the same contract.
- Third-party review round 1 (6 Medium, 4 Low, 0 High) and their
  dispositions — every finding verified against the code before
  acceptance, all fixed:
  - `LaunchOneHot` pinned its launch row from the LABELS, which the
    dispatcher stages to host, while the minted output lives on the
    scope's device — under a Push(d≠0) scope the kernel ran on device
    0 writing device-1 memory (the illegal-address class, exactly
    the mnist_ddp shape). Now pinned from `dst.device`; audited the
    whole 26-launcher pin table — oneHot was the only row read from a
    staged operand. Cross-device legs (in-scope oneHot, in-scope
    strided toDevice) added to the cross-device fixture; they run on
    the box (locally the fixture skips with the census reason).
  - The scatter/scatterAdd empty-index guards skipped the whole body,
    but the rank-1 value form still owes the dense span copy that
    seeds the result (the count gate guards only the axis form, so
    the empty index set is legal and the host path returns a copy of
    `a`): the device mint came back unseeded. Red fixture
    `op_tensor_scatter_empty_index` (cuda actual=2, cpu green) →
    guards narrowed to "skip the index work, not the copy".
  - `g_launchDevice` (the per-entry row selector) was process-global
    while tensor natives run on the VM's blocking channel without the
    VM lock — two carriers inside entries raced the pin. Now
    `thread_local`; the Push/Pop scope stack stays process-global by
    design (the registry/plugin lockstep and spec §7's single-carrier
    enqueue contract — the stream layer's own concurrency contract,
    not a per-op accident; the audit found no other shared state the
    CUDA stream/pool APIs do not already serialize).
  - `toDevice(stridedForeign, d)` inside a Push(d) scope raised the
    COMPUTE scope gate from its own internal densification — the
    advice in the message ("move them with tensor.toDevice") named
    the very call being made. Ruling: the gate guards CLASS 1 compute;
    a transfer is CLASS 2, owns its ordering through the hook, and is
    the sanctioned way to CROSS a scope — its internal densification
    is source-routed and ungated. Implemented as
    `MaterializeForTransfer` (the ungated source table) beside
    `MaterializeByResidency` (gated, for compute callers); box leg
    added (transpose view → in-scope toDevice → values arrive
    transposed).
  - Missing plan-mandated guards: the toDevice(a, device) census
    e2e (`op_device_transfer_guard` + mirror twin, .flags-pinned —
    first-absent-id and negative-id raises naming the count, runs on
    every machine) and the copyD2D peer-vs-staging branch pin
    (`CrossCopyUsesPeer` extracted to the shared header, table pinned
    in `test_native_tensor`).
  - Lows: the design doc's stale pre-M-G3 `toDevice(a)` sentences
    (§1/§3 now carry the scope-following ruling and the shipped
    census-raise wording), the 1 MiB staging magic number (now
    `DEFAULT_STAGE_CAPACITY_BYTES`), the transfer file header's ABI
    v2 wording (v3 with the copyD2D entry named).
  - Post-fix verification: bench Release full-tree rebuild, debug
    `test_native_tensor` 3/3 (the new branch pin included),
    `source_size_guard` green, the guard batch 6/6 on the cpu corpus
    leg (both twins, mirror via .flags), and the 212-line cuda
    double-run roster green (the two new census rows included, the
    cross-device fixture skipping with its census reason).
- Third-party review round 2 (the same delta re-audited: all ten
  round-1 dispositions individually verified fixed; 1 new Medium,
  3 new Low, 0 High) and their dispositions — every finding
  verified against the code before acceptance, all fixed:
  - `g_stageBuffer`/`g_stageCapacity` — the staging path's pinned
    host bounce — were process-wide and unsynchronized. Verified
    new in this delta (`git log -S` empty), and the M-G3
    cross-device path made it the ORDINARY route: the acceptance
    box reports `canAccessPeer = 0` both directions (design §6), so
    every `toDevice(a, device)` crosses through `CopyStaged`. The
    race is the same class the thread-local row pin exists for —
    two carriers inside transfer entries interleave their payloads
    in one bounce, and one carrier's doubling growth can
    `cudaFreeHost` a buffer the other's D2H has not drained. Now
    `thread_local`, the same model as the row pin: within one
    thread the copies are blocking (growth never races a drain) and
    the per-thread buffer is kept under the same keep-resources
    discipline as the row handles.
  - `CopyPeer` reused the destination row's `stagingDone` event for
    its cross-device handoff — the same row-shared event the
    M-G2-era bulk `CopyBytes` path records, so two concurrent
    transfers interleave handoffs (one carrier's
    `cudaEventSynchronize` observes the other's copy). Disposition
    covers both users: the bulk `CopyBytes` handshake and the
    `CopyPeer` handoff each create a PER-CALL event, ride it
    record/wait/destroy in the `CudaStreamJoin` join-event pattern
    (destroying after both streams have consumed its records is
    safe — the wait keeps the fence without the handle), and the
    row-shared `stagingDone` is deleted outright (state field,
    `CurrentStagingEvent` accessor, header declaration, row-init
    creation). `stagingStream` stays shared: its FIFO ordering IS
    the overlap contract; the event was per-transfer state
    misfiled into the row.
  - Terminology drift on the same interface pair: the manual's
    "zero-argument spelling"/「零参写法」 against CHANGELOG and
    `tensor.n`'s "one-argument spelling" — both names wrong in
    opposite directions (`toDevice(a)` takes one argument; only
    `streamCreate()` is argument-less). Unified to "the spelling
    that omits the device"/「省略设备的写法」 across the current
    feature text: both manual languages (two sites each), the
    CHANGELOG pair, the `tensor.n` native-declaration comments.
    The historical CHANGELOG entries about zero-argument
    CONSTRUCTION stay as they are — there "zero-argument" is the
    standard term.
  - The roster tally bullet conflated the runner's verdict count
    with the roster's line count (verdicts are per manifest entry,
    across both the main manifest and the examples manifest the
    runner scans); reworded to keep the two counts separate — the
    stated mechanism was itself wrong and re-corrected in round 3.
  - Post-fix verification: tensor_cuda plugin rebuild (the delta is
    plugin-internal), the 212-row cuda double-run roster re-run
    green on the same tally as the pre-fix run, and the guard
    batch re-checked.
- Third-party review round 3 (the round-2 fix round re-audited: all
  four dispositions verified fixed — the thread-local bounce, the
  per-call handoff events with the row-shared event deleted
  (leak-free return paths and the safe destroy-after-consumption
  lifetime confirmed against the `CudaStreamJoin` precedent), the
  terminology unification, and the ledger; 0 Medium-or-above, 3
  new Low, 1 Info — the loop closes here per the "某轮仅Low/Info
  级、修正后测试全过即可进入下一阶段" rule) and their
  dispositions, all verified against the code before acceptance:
  - the `CopyStaged` header comment still promised the
    process-wide bounce the fix had retired; reworded to the
    per-thread contract (a missed site — the neighboring
    `CurrentStagingStream` comment had been updated).
  - the zh manual's new stream-scope sentence used the colloquial
    「别个设备」; standardized to 「别的设备」 (the only such site
    in docs/, grep-verified).
  - the tally note's MECHANISM was misattributed (the round-2
    correction replaced the conflation with a wrong causality):
    the six fixtures double-listed in `manifest.txt` are
    double-listed in the roster too and cancel; the +1 verdict
    over roster lines is `class_basic`, listed in both
    `manifest.txt` and `examples_manifest.txt` (the runner scans
    both — verified against the log: `class_basic` carries two
    verdicts). Reworded above.
  - Info, ACCEPTED as-is: `CopyPeer` returns before the
    destination-stream wait when the per-call event cannot be
    created, leaving the already-enqueued peer copy unordered into
    the destination row. The pre-existing `CudaStreamJoin` accepts
    the same shape for the same effectively-unreachable handle
    OOM, and the anti-over-defense rule keeps the plain status
    raise over a source-row-sync-plus-blocking-copy fallback.
  - Residual-risk note, carried to the P1-g box acceptance: the
    fixed cross-device paths (staging bounce, peer handoff,
    census/gate legs, the 64 KiB-threshold handshake) execute on
    no single-GPU machine — the 2×4090 box is their first real
    run, and the box steps (corpus double-runs, the cross-device
    fixture with the oneHot/strided legs) are their acceptance.
  - Post-fix state: comment/wording-only since the round-2
    verification (header comment, one zh word, two ledger notes);
    the plugin is rebuilt for binary consistency while the
    functional gates stay as verified in the round-2 round.
- M-G3 box landing (2026-11-26, the 2×4090 acceptance machine at
  tree 9d3f0451, cuda/Debug/sm_89, tests off so the legs run through
  the e2e runner directly): the full cuda roster ran twice — 213
  passed, 0 failed, 0 skipped on both runs, and the verdict lists are
  byte-identical (the run-to-run determinism contract the corpus
  carries, now demonstrated with the cross-device transfers live).
  `op_tensor_cross_device` PASSED on two real devices — the first
  real run of the fixed staging/peer paths, including the oneHot and
  strided-view transfer legs and the census-guard branch below
  threshold. The peer branch itself still executed on no machine:
  the box reports `canAccessPeer = 0` both directions, so the box's
  ordinary cross-device path is the staging bounce and `CopyPeer`
  stays accepted on code review (round-2/round-3). Single-GPU
  regression: mnist_mlp ran from the same tree, exit 0 with test
  accuracy 0.896 against the locally measured 0.897 — float
  reduction-order noise on the accuracy's argmax compare, the
  baseline gate (0.85) held with wide margin. The Windows/
  single-GPU side of the M-G3 acceptance — the reported-skip legs
  and guard paths — was verified in the local rounds (skip
  sentinel + stderr reason).

## 5. M-G4 — mnist_ddp.n acceptance

Program shape (examples/mnist_mlp/mnist_ddp.n, the spec §8 split):

- one `tensor.seed(s)`; data loader reused from main.n; per batch
  the HOST batches split at a fixed row boundary; shard0 =
  `toDevice(hostBatch, 0)`, shard1 = `toDevice(hostBatch, 1)`
  (window assignment follows this fixed enqueue order — spec §7);
- weights created on device 0 (the plain creation path), every
  parameter tensor re-broadcast once per run (for a device-0
  tensor `w`, `w1 = toDevice(w, 1)`); the hyperparameters — batch
  32, 40 epochs, the same seed — match `main.n` exactly so the
  curves are comparable; per step: `streamCreate(0)`/
  `streamCreate(1)` Push scopes run forward/backward per device
  (the two chunk streams overlap), `streamJoin` both, then the
  gradient reduce `g = (g0 + toDevice(g1, 0)) * 0.5f` (device-0
  compute), SGD step on device 0, `assignInto(w1, toDevice(w, 1))`
  re-broadcast. The `0.5f` is load-bearing (round-1 finding):
  `backward` mean-scales by its shard's row count (`layers.n:64-70`,
  `float inv = 1.0f / batch` with `batch = probs.shape()[0]`), so
  the raw sum of two shard-means is 2x the full-batch gradient and
  silently doubles the effective learning rate. The mean-of-
  shard-gradients form is DDP's standard allreduce-mean semantics
  (the mainstream reference: PyTorch DDP averages gradients across
  ranks), and with equal-size shards it equals the full-batch mean
  gradient up to float order;
- prints the per-epoch loss lines in `main.n`'s existing format
  (`epoch N: loss L`, `main.n:85`) plus the final accuracy line
  (`main.n:91`), so the ddp run is directly comparable to the
  seeded single-GPU baseline.

Build wiring (round-1 finding — the example is not auto-built by
the corpus): `examples/mnist_mlp/mnist_ddp.n` with a sibling
`mnist_ddp.nproj` (Sources: `mnist_ddp.n`, `data.n`, `layers.n` —
the module pair is reused in place; `main()` lives in
`mnist_ddp.n`), and ctest entries `mnist_ddp_compile` +
`mnist_ddp_run` following `mnist_mlp`'s compile+run DEPENDS pair
beside `mnist_mlp_run` (`tests/CMakeLists.txt:2692`; `TIMEOUT 300`
mirrors that entry's calibrated ruling), and their timing rows land
through `gate_time_check.py record` (the new-entry duty), the run
entry carrying `SKIP_RETURN_CODE 90` so the skip lands in ctest's
native Skipped state. On cpu/single-GPU legs the program prints the
reasoned skip line and returns the sentinel — §1's skip contract;
the box's two-GPU cuda leg is the real run.

Acceptance (box):

- `tools/gate_multigpu.sh` (repo tools/, runs on the box): fresh
  cuda build → the double-run subset (M-G3 entries included, both
  runs byte-identical) → `mnist_ddp_run` twice: identical loss
  sequence; final accuracy >= 0.89 and within 0.01 of the seeded
  single-GPU baseline (0.896); the per-epoch loss curve within a
  band of the same-seed single-GPU baseline, which the gate
  re-runs fresh in the same script — spec §8's "float tolerance of
  the reduction-order change" is not a number until measured, so
  calibration is one-off: the first ddp-vs-baseline run records
  the observed max deviation and the band constant, set just above
  it, is committed in the gate script (a calibrated cap like the
  branch's timeout caps, not a mathematical assertion); and an nvidia-smi sampler showing GPU-Util > 0 on
  BOTH device indices during the run (the spec §8 occupancy
  check).
- The multi-GPU leg joins the box's gate script family next to
  `gate_cuda_tree.sh`.
- Docs: the example joins the manual's examples section (en/zh) +
  CHANGELOG; VERSION bumps to 0.14.13 — a new shipped example is a
  user-visible addition under AGENTS' rule even with no new symbol
  in the stdlib surface; manual sync duty per AGENTS.

M-G4 implementation record (2026-11-26):

- Program-shape deviations from the §5 sketch, under the user's
  2026-11-26 ruling 不需要向下兼容，合理性优先 (and KISS):
  - Per-step rebroadcast is a field reassignment —
    `p1.w1 = tensor.toDevice(p.w1, 1)` × 4 — not the sketched
    `assignInto(w1, toDevice(w, 1))`. Nothing reads a device-1
    handle across steps (each step's forward consumes the fresh
    fields, superseded arrays are reclaimed on their owning row),
    so the stable handle the assignInto pair existed to keep buys
    nothing — and the pair doubles the per-parameter transfer.
  - No device-0 side stream: the device-0 shard runs outside every
    push scope, so its ops land on device 0's main stream and the
    device-0 chain orders itself. An earlier revision of this note
    justified a device-0 Push with "handle 0 is the main stream" —
    wrong on a device backend: `streamCreate(0)` skips slot 0
    (`CudaStreamCreate` treats slot 0 as the device's main stream,
    "not pool") and returns the first pooled handle, so that draft
    ran the device-0 shard on a side stream with two undefined
    edges (side-stream writes read on main without a join, main
    writes read on the side stream without a fork). Review round
    1's High, confirmed against the code. `streamJoin(s1)` remains
    the program's one ordering edge — the direction the join
    defines — and transfers are blocking, which covers the rest.
  - Streams are created once (before the epoch loop) and destroyed
    at the end, pushed per step; per-step create/destroy would
    churn the pool slot for no gain.
- `tools/gate_multigpu.sh` landed in the box's gate-script family
  (probe → legs → GATE verdict line, SKIP exit 0 below two
  devices). Two script bugs the box round surfaced and fixed: a
  package derives its module name from the npkg file stem, so the
  dot-joined npkg names built from the stem string were invalid
  module names (the npkg names join with underscores now, the log
  names keep the dots); and the accuracy awk's multi-line condition
  is rejected by the box's awk (a single-line abs-diff form now,
  mirroring the band check's shape).
- Calibration (one-off per §5): fresh in-script baseline accuracy
  0.896; ddp double-run 0.896/0.896 with a byte-identical epoch
  sequence, and the max per-epoch deviation against the baseline
  under printed precision (< 1e-6). Band constant committed as
  `DDP_LOSS_BAND=0.001` — three orders above the observed maximum,
  two below the loss scales that matter here.
- M-G4 box landing (2026-11-26, build-cuda from the synced tree):
  GATE multigpu PASS — cuda rebuild, two-device probe, roster
  double-run 213/213 twice with byte-identical verdict lists and
  zero skips, baseline re-run exit 0 at 0.896, ddp twice exit 0
  with identical loss sequences and the accuracy gates met (floor
  0.89, 0.01 margin against the fresh baseline), loss band
  over=0/40, and the nvidia-smi sampler saw both device indices
  busy during the ddp runs (peak util 14%/11%). The Windows legs:
  `mnist_ddp_compile` PASSes (0.94 s, timing row recorded),
  `mnist_ddp_run` lands in ctest's native Skipped through
  `SKIP_RETURN_CODE 90` (its permanent-skip disposition is the
  note appended to the tail of `gate_time_reference.txt`).
- Docs shipped with the milestone: examples README rows (en/zh),
  the 19-tensor.md stream-section example paragraph (en/zh),
  CHANGELOG 0.14.13 (en/zh), VERSION 0.14.13. Limitations check:
  nothing to lift — the fault-reporting limitation stands
  untouched by an example-only change.

M-G4 review round 1 disposition (2026-11-26, third-party review of
the uncommitted milestone diff):

- High — the device-0 scope named a side stream, not the main
  stream (the rewritten bullet above). Fixed: the program opens no
  device-0 stream and pushes no device-0 scope; its header and
  inline comments state the real stream shape. The box's double-run
  greenness before the fix was enqueue-timing luck at 11–14 %
  utilization, not evidence — the box re-run with the corrected
  program is this disposition's verification step.
- Medium — this ledger's "handle 0 is the main stream"
  justification: fixed by the bullet rewrite.
- Medium — the user manual glossed device `0` as "the main stream"
  (19-tensor.md stream section, en/zh), the source the program's
  misreading grew from. Fixed: the sentence now separates the
  device selector `0`, the created handle (a stream of its own on
  that device), and the main stream's own handle `0`; mirrored
  en/zh. The same gloss sat in the `streamCreate(int device)`
  interface comment in stdlib/tensor.n — corrected there too
  (found by the independent read, not listed by the review).
  CHANGELOG 0.14.13 carries the Fixed entry (en/zh); the historical
  0.14.11/0.14.12 entries are left as records.
- Low — the manual's example paragraph was in implementer voice
  ("works the same surface", "census"; zh 普查值): reworded to
  "exercises the same stream and device surface" and the plain
  device-count wording.
- Low — the program's baseline comment quoted a stale measurement:
  it now names both (0.897 single-device, 0.896 on the box) and
  attributes the wobble to the last argmax compare.
- Low — the gate script misdiagnosed a missing stem directory (the
  first redirect fails and surfaces as "build broke"): the stem is
  resolved absolute once up front and its directory created, which
  also removes the `$root/<absolute-stem>` double-slash join.
- Verified safe, no change: the in-scope
  `toDevice(foreignOperand, d)` transfer (by-design gate exemption,
  pinned by op_tensor_cross_device.n); blocking transfers giving
  payload residence at return; the join's direction; the numerics
  parity (the load-bearing 0.5f scalings); the ctest entries and
  the timing row.
- Box re-landing with the corrected program and script (2026-11-26,
  the verification step the High's disposition named): the full
  multigpu gate leg end to end — GATE multigpu PASS. Roster
  double-run 213/213 twice, byte-identical verdict lists, zero
  skips; fresh baseline 0.896; the corrected ddp twice exit 0 with
  byte-identical 40-epoch sequences (epoch 1 prints 1.4117792876…
  against the baseline's 1.4117792979… — reduction-order noise at
  the 8th decimal, inside the 0.001 band at over=0/40, printed
  maxdev 0.000000) and accuracies 0.896/0.896; the sampler saw both
  device indices busy, peak 26%/11% — device 0's share is higher
  than the pre-fix 14%/11%, consistent with its shard now running
  the full loss/allreduce/SGD chain on the main stream. The leg's
  first launch this round failed on the stem-normalization fix
  itself — the `train_run` helper still joined the now-absolute
  stem onto `$root` — caught by the leg's baseline step, fixed, and
  the passing run exercised the corrected script whole.

M-G4 review round 2 disposition (2026-11-26, re-review after the
round-1 fixes):

- All six round-1 findings re-derived from the code independently
  and confirmed fixed and complete — the reviewer's edge-by-edge
  ordering walk reaches no remaining undefined ordering at the
  ABI level. Zero new Medium+.
- Low — a stray mid-sentence line break in the M-G3 box-landing
  bullet: rewrapped.
- Low — the program header cited design §8 for the stream shape
  after its device-0 half deviated: the citation now names the
  plan §5 M-G4 record as the revision it follows.
- Info — the timing row predated the final program trim:
  re-recorded from a clean single-entry run afterwards (0.94 →
  0.95 s, updated=1, recorder PASS).

## 6. Risks and mitigations

- **Carrier threads / contexts**: every plugin entry pins the
  device per call (M-G1 step 3) — no per-thread current-device
  assumption survives; the corpus's carrier-heavy tests re-run at
  every milestone's gate.
- **Handle renumbering** (cuda first chunk handle 0→1): round-3
  review verified opaque-only usage across the corpus; M-G1's
  corpus gate is the standing check.
- **Pinned staging buffer**: process-lifetime, grow-doubling,
  freed at exit; the D2D sizes in this line are batch/gradient
  arrays (MB scale), not whole datasets.
- **Interleaved Push/Pop across devices**: the plugin push stack
  restores per-device currents per entry (M-G1 step 6); covered by
  a dedicated unit test.
- **PRNG**: zero change (spec §7); the corpus's seeded tests are
  the guard.
- **Old plugin/host mismatch**: host ABI v16 + device ABI bump are
  load-time rejections, never runtime mysteries.
- **Debug/ndb**: the print/staging arms carry the device arg
  through the same hooks (spec §10.3) — no ndb surface work.

## 7. Verification matrix

Docking policy (user ruling 2026-11-26: intermediate milestones
must not sit under the full gates — they cost hours and slow the
line down; heavy legs run ONCE, at the line's single convergence):
M-G2/M-G3 dock at the T0 shape (Debug unit suites of the touched
trees + the targeted e2e families + the new entries themselves)
plus the device net on the box; the full Windows T1 (all five
legs), the asan legs and the T2 additions run at M-G4 with the
release convergence. M-G1 is exempt from the relaxation — its
full dock already ran (the line's riskiest layer is ABI/VM
surgery, already docked before the feature stages start).

| leg | M-G1 | M-G2 | M-G3 | M-G4 |
| --- | --- | --- | --- | --- |
| Windows dbg build+unit (M-G2/3: touched trees + new entries) | full | targeted | targeted | full |
| Windows cpu e2e (M-G2/3: tensor/device/stream families) | full | targeted | targeted | full |
| Windows asan build+unit, asan e2e subset | yes | — | — | — (slim close, see note) |
| Windows cuda bench build (tree/double-run only at M-G4) | tree+double | build | build | tree+double |
| Windows full T1 (five legs, one batch) | yes | — | — | slim close (see note) |
| box cuda build + double-run subset + new e2e | yes | yes | yes | yes |
| box mnist single-GPU regression | yes | yes | yes | yes |
| box gate_multigpu.sh (ddp, twice) | — | — | — | yes |
| T2 close (slim: Debug units, ASan full e2e, Release build/e2e, docs five legs) | — | — | — | yes |

Close shape (owner ruling 2026-11-26, 加快速度、避免重复测试,
implemented in gate_tier.sh's t2 and recorded in
gate_tiering_design.md): the convergence batch runs Debug full
units + ASan full e2e + Release build and Release e2e + the docs
five legs. Debug full e2e is redundant with the ASan pass (same
codegen, the instrumented run adds memory checks), the ASan subset
is a literal subset of the full pass, and the IDE tree / ASan
units cover their test families already through the Debug suite —
they ride the T1 docks when the diff surface makes them relevant.
Release keeps build AND e2e (optimized codegen is the one regime
without another functional check). The tier coverage invariant is
untouched: the e2e manifest stays fully selected by T1 and by the
ASan full pass.

The box stays per-milestone: it is the only machine where the
line's subject matter (devices) executes at all, and its legs run
fast on 64 cores. Intermediate commits iterate at T0 (impact
mapping, minutes); nothing that could hide a device regression
is dropped — cpu-e2e targeting follows the impact map, and the
tier coverage invariant still binds the final T1/T2 selections.
