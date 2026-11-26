/*---
    multi_gpu_design.md — the multi-GPU device line spec (reviewed;
    grounds: the CUDA Programming Guide's multi-GPU chapter, PRACE's
    multi-GPU CUDA course, PyTorch's explicit-device model, and the
    0.14.9 stream-orchestration design this line extends. Round 1:
    the draft's three open questions closed as the decisions in
    section 10, verified against the code. Round 2: independent
    third-party review (1 High + 8 Medium findings — mint-output
    routing, mirror device-count semantics, stream-handle encoding,
    scope/operand device pinning, view owner indirection, staged
    reserved-byte clearing, no-P2P acceptance box measured on the
    hardware, lazy-init peer enablement, and the Lows) applied.
    Round 3: the reviewer's re-review (3 Medium — cpu toDevice
    no-op, scope-raise quantification to device-resident operands,
    stream pool slot-0 reservation — and 3 Lows) applied.
---*/

# Multi-GPU design (device line v2)

## 1. Goals

- A program addresses the machine's GPUs by index and moves arrays
  between them explicitly; the existing single-GPU programs run
  unchanged (the default device stays device 0 and outside any
  stream scope `toDevice(a)` keeps its meaning; section 3 carries
  the in-scope refinement).
- All compute stays same-device (PyTorch's discipline, and the
  registry's existing same-residency rule generalized): operands on
  different devices raise by name — co-locating is an explicit
  `toDevice` move.
- Streams generalize to per-device pools; the 0.14.9 determinism
  contract (seeded runs reproducible) survives unchanged.
- The acceptance program trains mnist data-parallel across the box's
  2×RTX 4090: per-device replicas of the weights, per-device batch
  shards, one explicit gradient all-reduce written in the language
  surface (a `toDevice` move + sum — the runtime routes it peer or
  through host staging), deterministic and reproducible.

## 2. Non-goals (this line)

- No multi-process/NCCL collective layer (a later line; the
  acceptance is intra-process, the language's own concurrency model).
- No automatic data parallelism (no implicit batch sharding); the
  program states the split, the runtime provides the plumbing — same
  philosophy as the 0.14.9 stream-orchestration design.
- No unified/managed memory; device memory stays per-device pools.
- No device-side random fill: `uniform` keeps its host-fill shape
  (section 7), so determinism needs no device-side state.

## 3. Script surface (stdlib/tensor.n)

Three new entries, no reshaping of the existing ones:

- `tensor.deviceCount() → int` — the active backend's device count:
  cpu 0 (no device backend, nothing to address); mirror 1 (its
  virtual pool is one virtual device — which also makes the
  device-argument guards of this line testable on plain CPU CI);
  cuda the probe's count, stored in the registry's backend state at
  activation. It doubles as the portability guard: a program
  branches on `deviceCount() > 1` for the data-parallel shape and
  stays single-device otherwise.
- `toDevice(a, device)` — a new arity alongside the existing
  `toDevice(a)`:
  - host → device d: H2D as today;
  - device d0 → device d1 (d0 ≠ d1): a peer copy when the pair has
    peer access, otherwise an explicit host staging (the program
    reads the same either way);
  - same device: returns the operand itself, the same handle (the
    existing same-residency contract);
  - `device` outside `0 .. deviceCount()-1` raises by name
    ("tensor.toDevice: device <d> beyond the device count <c>");
    on cpu the new
    arity raises for every index (the range is empty) while the
    old-arity `toDevice(a)` keeps its cpu no-op return — the
    standing to_device contract: on cpu every array is already on
    the active device.
- `streamCreate(device)` — a new arity alongside the existing
  zero-argument `streamCreate()` (which keeps device 0). A stream
  belongs to the device it was created for, and a Push scope pins
  ops to (the stream's device, the stream). On cpu the handle is
  the same stream no-op as today (the PRNG chunk window still
  works) and only device 0 is accepted; mirror follows its
  `deviceCount() == 1`.

The old-arity `toDevice(a)` keeps its ensure-residency contract and
follows the stream scope: a host operand lands on the CURRENT
scope's device — device 0 outside any Push scope, exactly today's
behavior, and the scope's device inside one, so a scope owns both
the compute and the uploads that feed it (the M-G3 landing: a scope
that uploaded on another device could never consume its own upload
inside that scope); an operand already resident on any device
returns itself (the same handle). Single-scope code sees device 0
either way, which keeps existing programs' meaning unchanged under
multi-device.

`tensor.backend()` is unchanged (the backend name, not a device).

## 4. Residency, the device id and the ABI

- **Payload residency stays what it is** — `NativeArrayView.
  reserved[0]`, 0 = host, 1 = device (ABI v13). The device id rides
  the sibling byte `reserved[1]`, which every view writer already
  zeroes and no reader consumes: the id lands in the existing layout
  with no widening, and a single-GPU array's bytes are bit-identical
  to today (reserved[1] = 0 is both the filler and device 0). The
  invariant: `reserved[1]` is meaningful only when `reserved[0] ==
  1`, and the one staging choke point that flips `reserved[0]` to
  host clears `reserved[1]` in the same step, so no reader has to
  gate on the pair.
- **VM side**: the residency side vector `m_arrResidency` (0/1)
  stays a host/device flag — every `== 1` routing test in the VM
  (element accessors, staging, format, the host-native gate) keeps
  its meaning. A new device-id side vector, filled and cleared
  through exactly the same lifecycle as the residency vector (the
  free-list reuse guard, the sweep clear, the sentinel alignment
  discipline), carries the owning device per record. Keeping them
  as two vectors is a deliberate split: folding the id into the
  residency byte (id + 1) would silently turn every forgotten
  `== 1` test into a misroute.
- Exactly like the residency vector, the device vector is written
  for owner records only — a view record's own entry is never
  written; a view's device resolves through its owner at the same
  owner-indirection sites residency resolution already uses.
- `AssertSameResidency` generalizes to same class AND same device
  id (the error names both devices); the three payload classes
  (COMPUTE / TRANSFER / METADATA) keep their entry points. COMPUTE
  routes per the primary operand's device and launches on that
  device's current stream; inside a Push scope, a *device-resident*
  operand whose device differs from the scope's stream's device
  raises by name — the join contract lives on the scope's stream,
  and quietly routing an op onto another device's main stream
  would under-wait the join. Host operands keep running the cpu
  table in any scope: nothing launches on a stream, the join
  contract is untouched (the 0.14.9 shape). Outside any scope,
  device-resident ops ride the operand device's main stream
  (today's behavior, generalized).
- Minting splits along the existing contract: `MintArray` follows
  the current device — the Push scope's device, or 0 outside any
  scope (language-level creations inside the VM know no scope and
  land on 0 by the same rule); `MintOutputLike` follows the
  *operand's* device — the standing "the output follows the
  operands' residency" ruling, widened from the residency class to
  the device id, because the kernel writing that output launches on
  the operands' device.

## 5. The device parameter at the boundaries

The VM and the plugin today assume one device everywhere a pointer
crosses a boundary. The fix rule is one change per boundary — the
id rides the parameter list, never hidden plugin state (an implicit
"current device" is state a later reader cannot see):

- `newArrayResident` widens in place with a device argument and a
  NativeHost ABI-version bump — the entry sits mid-table, and
  module load already hard-rejects an ABI-version mismatch, so
  widening is safe. The tensor native decides the device
  (section 4's mint rule), the VM stores it in the device vector
  and passes it down.
- The `TensorDeviceHooks` set the VM routes device records through
  (alloc / zero / free / copyH2D / copyD2H) gains the device id as
  an argument: every call site already knows it from the device
  vector (the element accessors, the staging arms, the formatter,
  the sweep release), while the mint site hands `alloc` the value
  straight from the `newArrayResident` argument — the vector entry
  is written once the payload exists. The plugin sets the context
  per call instead of guessing from the pointer. The `sync` entry keeps its
  signature — the VM never calls it; the plugin's launchers own
  that discipline and it is device-correct through the stream the
  launcher launched on. The mirror backend registers the same
  entry set and
  widens with it.
- The ops-table slots need NO signature change: every slot already
  takes its operands as `NativeArrayView` references, and the id
  rides their `reserved[1]` — the dispatcher asserted same-device
  upstream, the output mint follows the operands, so the plugin
  pins the context from the operand view and picks that device's
  current stream. (Widening ~40 slots × 3 tables to re-carry what
  the views already carry would be ceremony.)
- `streamCreate` gains a device argument (it is the one stream call
  without a handle to decode yet); `streamPush/Destroy/Join` need
  no signature change because the stream handle itself encodes its
  device — `device * kNativeMaxStreams + slot`, documented in the
  tensor ABI header.
  `streamPush/Destroy/Join` carry only the handle and the registry
  keeps no per-handle state, so without the encoding
  `streamCreate(0)` and `streamCreate(1)` would collide on the same
  slot number. The encoding comes with a pool-role ruling: slot 0
  of every device pool is that device's implicit main stream,
  never handed out by `streamCreate` — today's cuda pool hands
  slot 0 to the first chunk stream and keeps its main stream
  private, so without the ruling handle 0 would mean "main stream"
  on cpu and "first chunk stream" on cuda, the exact backend-split
  meaning this line removes elsewhere. With slot 0 reserved,
  handle 0 decodes to (device 0, main stream) on every backend and
  the cpu/mirror no-op sentinel lands on the same decoded meaning;
  programs move handles opaquely, so cuda's renumbering (first
  chunk handle 1) is program-invisible — the M-G1 corpus gate
  verifies that (verified against the corpus by the round-3
  review: every stream e2e treats handles as opaque).

The registry's single `DeviceBackendState` stays the front — the
language sees one backend with N devices, not N backends. The
descriptor does NOT grow a per-device table of pointers: the plugin
keeps its per-device state private and interprets the device
argument at the entry — context pins, per-device stream pools with
their implicit main streams, allocation pools, event pools, and the
device-0 singletons of today that carry error attribution: the
kernel error-flag buffer, the staging stream/event pair and the
sticky fault register (they join the table too, or a device-1
launch misreports its fault into device 0's state). Activation
also copies the probe's `deviceCount` into the registry's backend
state — the device-argument guards need it, and today the probe
result is checked and dropped.

## 6. Cross-device transfers

- On enablement: at first device use — the plugin's lazy init, the
  standing D4 ruling keeps context creation off the probe path and
  enabling needs a current context on the source device — query
  `cudaDeviceCanAccessPeer` for every ordered pair (i, j); where
  yes, `cudaDeviceEnablePeerAccess` (the call is one-way per pair;
  the ordered-pair loop covers both directions). The matrix is
  fixed from that point (topology does not change under a running
  program) and queryable per pair.
- D2D copy: `cudaMemcpyPeerAsync` between the two devices' streams
  when peer is on — the copy is enqueued after the source operand's
  producing work, an event is recorded on the source device's
  stream, and the consuming device's stream waits on it before any
  op reads the copy. Ordering is explicit at both ends: a peer copy
  is a cross-device handoff, not an atomic move.
- Staging fallback when peer is off: D2H on the source device's
  stream → H2D on the destination's, pinned host staging, the same
  event handoffs at both ends. Measured on the acceptance box (2×
  RTX 4090, driver 580.173.02, same-NUMA PCIe): `canAccessPeer` is
  0 in both directions — GeForce Ada has no peer access at all, so
  staging is the box's ordinary path, not an edge case (measured
  there; peer stays the fast path on peer-capable systems, and the
  fallback's correctness is what the language surface promises).
- `streamJoin` semantics across devices: a join on a stream whose
  peer work spans devices records the needed events on each source
  device's stream — the handoff ordering contract stays "the main
  stream sees everything the joined stream wrote".
- Coverage honesty: no machine in the gate fleet has a
  peer-capable pair (the dev box is single-GPU, the acceptance box
  measured 0/0 — above), so the `cudaMemcpyPeerAsync` branch gets
  compile-time and per-pair-query coverage only and no e2e leg can
  execute it. The staging fallback carries the language-level
  promise everywhere the fleet runs, and that is what acceptance
  measures; an unreachable branch, stated — not a silently
  uncovered one.

## 7. Determinism (the 0.14.9 contract, unchanged)

Evidence from the code fixed this section: the random fill is
**host-side** end to end. The engine is VM-owned (one MT19937 under
the RNG mutex; `snapshotWindowRng` draws from a private copy), the
chunk-window cursor is the tensor native's process-global
`g_chunkWordCursor`, and a device `uniform` fills a host twin that
lands in one H2D block. Window assignment therefore already happens
at enqueue time in host program order, one global sequence,
disjoint by construction — device-parallel draws are disjoint and
reproducible with no change to the mechanism, and a single-GPU
program's draw sequence does not move at all.

- No per-device tiling: it would only renumber the windows the
  second device draws without buying determinism (one cursor
  already separates them), and it would make the sequence depend on
  how many devices the run happens to use.
- The precondition the contract always had stands: windows follow
  host enqueue order, so a data-parallel program enqueues its
  shard-creation calls in a fixed order (the acceptance does, from
  one carrier).
- A device-side fill kernel (counter-based draws landing directly
  on each device) is a later optimization; if it arrives, it must
  draw from the same global window sequence per device to keep the
  contract — out of this line's scope.

## 8. Acceptance program

- `examples/mnist_mlp` gains `mnist_ddp.n` (the language-level
  data-parallel shape): weights replicated to both devices, the
  batch split into per-device shards, forward/backward per device
  in a Push scope, the gradients all-reduced by an explicit
  `toDevice` move + `add`, the SGD step applied on device 0 and the
  weights re-broadcast. Deterministic (seeded) and accuracy-matched
  against the single-GPU run.
- Acceptance gate: the run trains on 2 GPUs, the loss curve matches
  the seeded single-GPU baseline within the float-tolerance of the
  reduction-order change, and nvidia-smi shows both devices busy.
- Regression guards ride along: the existing single-GPU mnist run
  and the full tensor e2e corpus (device-0-only programs, the
  bit-identical claim of section 4) stay green, and a cross-device
  op pair raises by name with both devices named.
- Machine-dependent gating: the cross-device vector and the
  cross-device raise run only where `deviceCount() >= 2` (an
  environment skip elsewhere — the gate's coverage selection still
  lists them); the range guards (out-of-range raise, old-arity
  return-itself) run on every machine through mirror's
  `deviceCount() == 1` — mirror's stream space is the fixed
  handle 0, so a handle encoding with a non-zero device needs
  `deviceCount() >= 2` too.

## 9. Milestones

Each milestone landing its user-visible entry also lands the
paired VERSION bump and manual/CHANGELOG update (the standing sync
duty): M-G2 the `deviceCount()` and `streamCreate(device)`
entries, M-G3 the `toDevice(a, device)` overload.

- M-G1: the ABI + boundaries (the device-id side vector,
  `reserved[1]`, the device arguments on `newArrayResident` and the
  device hooks, the stream entries — device on `streamCreate`,
  handle-encoded devices elsewhere —, `AssertSameDevice`; the ops
  tables ride their operand views unchanged; single-GPU programs
  bit-identical, the whole existing corpus green — all its programs
  are single-device, all on device 0).
- M-G2: per-device pools in the plugin (contexts, streams, alloc);
  `deviceCount()`; the e2e corpus green on device 0.
- M-G3: cross-device transfers (`toDevice(a, d)`, peer matrix,
  pinned-host staging fallback); a cross-device e2e vector on the
  2-GPU box (which exercises the staging path — section 6).
- M-G4: the acceptance program (`mnist_ddp.n`) + the multi-GPU gate
  leg on the box.

## 10. Reviewed decisions (were the draft's open questions)

1. **Device id representation — second byte, not a widening.**
   `reserved[1]` of `NativeArrayView` is zero-filled by every writer
   and never read; the residency byte and the VM's `== 1` routing
   tests stay untouched (sections 4-5). Verified against
   NativeHost.h:247, VmExecutorNativeArray.cpp:64/101 and
   NativeHostHelpers.h:71-72.
2. **No PRNG change.** The window mechanism is host-side (the VM
   engine, the native's window cursor, host-fill + one H2D —
   tensor_random.cpp), so the single global sequence already covers
   multi-device draws deterministically; per-device tiling is
   rejected (section 7). No single-GPU program can regress: its
   sequence is byte-for-byte the old one.
3. **ndb stays out of this line.** The debug/print paths already
   stage device records through the device hooks (the formatter's
   `StageDeviceArrayForFormat` arm), and the device argument makes
   those hooks device-correct for free — acceptance needs no
   device-aware inspection surface beyond printing, which works.
