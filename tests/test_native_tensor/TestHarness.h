/*---
    TestHarness.h — shared CHECK/PASS/FAIL counters for the
    test_native_tensor suites (same convention as tests/test_vm).
    Each suite returns its failure count; main() aggregates.
---*/
#pragma once

#include <iostream>

namespace test_native_tensor {

extern int g_pass;
extern int g_fail;

} // namespace test_native_tensor

#define NTT_TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while (0)
#define NTT_PASS() \
    do { ++test_native_tensor::g_pass; std::cerr << "OK\n"; } while (0)
#define NTT_FAIL(msg) \
    do { ++test_native_tensor::g_fail; std::cerr << "FAIL: " << msg << "\n"; } while (0)
#define NTT_CHECK(cond, msg) \
    do { if (!(cond)) { NTT_FAIL(msg); return; } } while (0)
