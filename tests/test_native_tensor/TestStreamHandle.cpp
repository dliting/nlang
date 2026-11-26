/*---
    TestStreamHandle.cpp — the stream-handle encoding contract
    (multi-GPU spec §5, the handle ruling):
    handle = device * kNativeMaxStreams + slot,
    with (device 0, slot 0) — handle 0 — the implicit main stream
    everywhere. The registry and the cuda plugin share the encoding
    through backend_descriptor.h's inline helpers, so the unit test
    rides the header itself (header-only: no plugin to load).
---*/
#include "TestHarness.h"
#include "backend_descriptor.h"

namespace test_native_tensor {

namespace {

void CheckEncodeDecodeRoundTrip() {
    NTT_TEST(stream_handle_round_trip);
    const int devices[] = {0, 1, 2, 3, 7};
    const int slots[] = {0, 1, 7, tensor::kNativeMaxStreams - 1};
    for (int d : devices) {
        for (int s : slots) {
            const int h = tensor::EncodeStreamHandle(d, s);
            NTT_CHECK(tensor::DecodeStreamDevice(h) == d,
                      "the device must survive the handle round trip");
            NTT_CHECK(tensor::DecodeStreamSlot(h) == s,
                      "the slot must survive the handle round trip");
        }
    }
    NTT_PASS();
}

void CheckMainStreamSentinel() {
    NTT_TEST(stream_handle_main_sentinel);
    //Slot 0 of device 0 is the implicit main stream: its handle is 0,
    //and 0 must decode back to exactly (0, 0).
    NTT_CHECK(tensor::EncodeStreamHandle(0, 0) == 0,
              "the implicit main stream's handle must be 0");
    NTT_CHECK(tensor::DecodeStreamDevice(0) == 0,
              "handle 0 must decode to device 0");
    NTT_CHECK(tensor::DecodeStreamSlot(0) == 0,
              "handle 0 must decode to slot 0");
    //Every other pair gets a nonzero handle — the plugin's first pool
    //slot is 1, and a pool handle never aliases the sentinel.
    NTT_CHECK(tensor::EncodeStreamHandle(0, 1) != 0,
              "the first pooled slot must not alias handle 0");
    NTT_CHECK(tensor::EncodeStreamHandle(1, 0) != 0,
              "device 1 must not produce handle 0");
    NTT_PASS();
}

void CheckForeignDeviceDecode() {
    NTT_TEST(stream_handle_foreign_decode);
    //M-G2: a foreign-device handle decodes faithfully — the push stack
    //reads the scope's device out of the top handle, so a handle made
    //on another device must name that device at any census position,
    //and decoding stays pure arithmetic (no clamp toward device 0).
    const int boundary = tensor::kNativeMaxDevices - 1;
    const int foreign = tensor::EncodeStreamHandle(boundary, 1);
    NTT_CHECK(tensor::DecodeStreamDevice(foreign) == boundary,
              "a census-boundary handle must decode to its own device");
    NTT_CHECK(tensor::DecodeStreamSlot(foreign) == 1,
              "a census-boundary handle must keep its slot");
    const int deviceOneSlot1 = tensor::EncodeStreamHandle(1, 1);
    NTT_CHECK(tensor::DecodeStreamDevice(deviceOneSlot1) == 1,
              "one step off device 0 must decode to device 1");
    NTT_CHECK(tensor::DecodeStreamSlot(deviceOneSlot1) == 1,
              "device 1 slot 1 must not alias device 0's slots");
    NTT_PASS();
}

void CheckPoolWidthPinned() {
    NTT_TEST(stream_handle_pool_width);
    //ABI-visible: the plugin sizes its per-device stream pools by this
    //constant, so a change is a contract change (device ABI bump).
    NTT_CHECK(tensor::kNativeMaxStreams == 16,
              "the pool width is pinned at 16 streams per device");
    NTT_PASS();
}

void CheckDeviceAbiVersionPinned() {
    NTT_TEST(device_abi_version);
    //v3: the copyD2D descriptor entry (multi-GPU M-G3) on top of v2's
    //streamCreate(int device) (M-G1). The plugin's
    //nlang_device_abi_version export must carry the same value — the
    //load-time probe rejects a mismatch, and this pin catches silent
    //drift on either side.
    NTT_CHECK(tensor::NLANG_DEVICE_ABI_VERSION == 3u,
              "the M-G3 device ABI is version 3");
    NTT_PASS();
}

void CheckDeviceAbiProbeMatch() {
    NTT_TEST(device_abi_probe_match);
    //The probe's pure decision (the load-time reject path, no stub
    //plugin needed): only the exact version matches — an older or a
    //newer plugin descriptor must be refused, not half-spoken.
    NTT_CHECK(tensor::DeviceAbiMatches(tensor::NLANG_DEVICE_ABI_VERSION),
              "the host's own version must match");
    NTT_CHECK(!tensor::DeviceAbiMatches(1u),
              "the v1 plugin (no device selector) must be refused");
    NTT_CHECK(!tensor::DeviceAbiMatches(2u),
              "the v2 plugin (no copyD2D entry) must be refused");
    NTT_CHECK(!tensor::DeviceAbiMatches(4u),
              "a future plugin version must be refused too");
    NTT_CHECK(!tensor::DeviceAbiMatches(0u), "version 0 must be refused");
    NTT_PASS();
}

void CheckCrossCopyBranchSelection() {
    NTT_TEST(copyd2d_branch_selection);
    //The copyD2D branch selection (device ABI v3, spec §6): the direct
    //peer copy launches from the source row's stream, so the decision
    //is the source row's enablement of the destination alone — a
    //half-enabled pair (consumer boards ship asymmetric rows) still
    //peers the enabled direction and stages the other through pinned
    //host memory; a fully disabled pair always stages. The plugin
    //routes CudaPeerReady through this function, so the table is
    //pinned without a device.
    NTT_CHECK(tensor::CrossCopyUsesPeer(true),
              "an enabled source-to-destination row takes the peer copy");
    NTT_CHECK(!tensor::CrossCopyUsesPeer(false),
              "a disabled row stages through pinned host memory");
    NTT_PASS();
}

} // namespace

int RunStreamHandleTests() {
    CheckEncodeDecodeRoundTrip();
    CheckMainStreamSentinel();
    CheckForeignDeviceDecode();
    CheckPoolWidthPinned();
    CheckDeviceAbiVersionPinned();
    CheckDeviceAbiProbeMatch();
    CheckCrossCopyBranchSelection();
    std::cerr << "streamhandle suites: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail;
}

} // namespace test_native_tensor
