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
    //v2: the streamCreate(int device) signature (multi-GPU M-G1). The
    //plugin's nlang_device_abi_version export must carry the same
    //value — the load-time probe rejects a mismatch, and this pin
    //catches silent drift on either side.
    NTT_CHECK(tensor::NLANG_DEVICE_ABI_VERSION == 2u,
              "the M-G1 device ABI is version 2");
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
    NTT_CHECK(!tensor::DeviceAbiMatches(3u),
              "a future plugin version must be refused too");
    NTT_CHECK(!tensor::DeviceAbiMatches(0u), "version 0 must be refused");
    NTT_PASS();
}

} // namespace

int RunStreamHandleTests() {
    CheckEncodeDecodeRoundTrip();
    CheckMainStreamSentinel();
    CheckPoolWidthPinned();
    CheckDeviceAbiVersionPinned();
    CheckDeviceAbiProbeMatch();
    std::cerr << "streamhandle suites: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail;
}

} // namespace test_native_tensor
