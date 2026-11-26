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
(the runner validates that list against the manifest) — this
branch has no `gate_time_check.py` / `gate_time_reference.txt`
(master-tree tooling whose per-entry timing rows cover ctest
entries, not e2e manifest rows), and new ctest entries follow the
existing calibrated-timeout convention in
`tests/CMakeLists.txt`; every user-visible
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
6. Plugin (cuda): `CudaStreamCreate(device)` range-checks against
   the probe count, reserves slot 0, encodes handles; push/join/
   destroy decode (a foreign decode mismatch is a plugin invariant
   raise); the push stack entries carry (device, saved current) so
   interleaved cross-device Push/Pop restore correctly;
   per-device `g_currentStream[d]` (still one device deep in M-G1,
   the table shape lands now so M-G2 only fills it).
7. Manual: nothing (the first draft's `formats/30-abi.md` does not
   exist on this branch — round-1 finding). M-G1 adds no
   user-visible surface, and no user-manual chapter documents the
   device-plugin contract: its authority is the
   `backend_descriptor.h` header comments (the handle encoding,
   `reserved[1]`, v16 entries land there). The manual's device
   chapter is `library/19-tensor.md` §19.7, first changed at
   M-G2. `formats/28-abi.md` is the native-module ABI — a
   different contract this line does not touch (its "version 12"
   stamp is pre-existing drift on this branch).
8. Full rebuild (both configs), Windows four legs, box cuda build +
   the cuda double-run subset (`tools/e2e_cuda_double_run.txt`,
   205 rows at branch state) + single-GPU mnist. Expected:
   everything green with deviceCount() never consulted — M-G1
   introduces no user-visible device choice.

Exit: corpus bit-identical behavior; every boundary carries the id;
the only files M-G2+ will rework are the plugin internals.

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
  manifest keys on, `manifest.txt:2370+`; skips unless
  `deviceCount() >= 2`,
  prints "skip"): create a on the default device, `b =
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
the `copyD2D` entry (§1); manual `19-tensor.md` §19.7 toDevice
overload (en/zh) + the known-limitations line review; CHANGELOG;
manifest row + double-run line for `op_tensor_cross_device.n`
(subset 208 rows); pins 502→506; SYNC_BASELINE. Box: cross-device
e2e green both runs; Windows/CI: the skip and guard paths.

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
mirrors that entry's calibrated ruling). On cpu/mirror legs the
program prints its skip line and exits 0 (`deviceCount() < 2`);
the box's cuda leg is the real run.

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

| leg | M-G1 | M-G2 | M-G3 | M-G4 |
| --- | --- | --- | --- | --- |
| Windows dbg build+unit, asan build+unit | yes | yes | yes | yes |
| Windows cpu e2e | yes | yes | yes | yes |
| Windows cuda build+tree+double-run | yes | yes | yes | yes |
| box cuda build + double-run subset | yes | yes | yes | yes |
| box new e2e (count/guards/cross) | — | yes | yes | yes |
| box mnist single-GPU regression | yes | yes | yes | yes |
| box gate_multigpu.sh (ddp, twice) | — | — | — | yes |

Every shared-source commit re-runs the Windows four legs; every
milestone ends with a full box rescan (build + subsets + new
entries) per the standing gate discipline.
