/*---
    test_file_time_epoch.cpp — unit tests for the bridge from a <filesystem>
    timestamp to Unix epoch milliseconds (src/native/fs/FileTimeEpoch.h).

    An e2e corpus can only pin time-independent facts about a real file's
    mtime: that the value is a positive epoch time and that the wall clock
    sampled afterwards is not behind it. Whether two writes of one file
    compare in order depends on how coarse the filesystem's timestamp is,
    which no corpus can force, so the arithmetic is pinned here instead with
    both clock samples injected. The bridge is pure, so every case below has
    one right answer regardless of the machine it runs on.
---*/
#include "FileTimeEpoch.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>

using nlang::native::FileTimeToEpochMs;

static int g_pass = 0, g_fail = 0;

namespace {

using Milli = std::chrono::milliseconds;
using Micro = std::chrono::microseconds;
using SysNow = std::chrono::system_clock::time_point;
using FileTime = std::filesystem::file_time_type;

// Reports both sides: "off by one millisecond" is the failure these cases
// exist to catch, and a bare condition would hide which side moved.
void CheckMs(const char* what, long long got, long long want) {
    if (got == want) {
        ++g_pass;
        return;
    }
    ++g_fail;
    std::fprintf(stderr, "FAIL: %s gave %lld ms, want %lld ms\n",
                 what, got, want);
}

// Scale: an exact-millisecond triple converts to an exact-millisecond answer,
// so a slip into microseconds or 100-nanosecond units fails loudly.
void TestExactMillisecondScale() {
    const SysNow systemNow{Milli(1000000000)};
    const FileTime fileNow{Milli(4000000000)};
    const FileTime fileTime = fileNow - Milli(1500);
    CheckMs("1500 ms back from a whole-millisecond sample",
            FileTimeToEpochMs(fileTime, systemNow, fileNow), 999998500);
}

// The sub-millisecond parts of the two durations cancel rather than each
// being floored on its own. The system clock sample sits 0.4 ms past its
// millisecond and the file sits 1.9 ms in the past, so their sum crosses a
// whole millisecond boundary: one truncation of the sum gives 999999998,
// while flooring each half gives 999999999 — the same unchanged file then
// answers two different values as the samples drift, which is what an
// unchanged file must never do.
void TestSubMillisecondPartsCancel() {
    const SysNow systemNow{Milli(1000000000) + Micro(400)};
    const FileTime fileNow{Milli(4000000000)};
    const FileTime fileTime = fileNow - (Milli(1) + Micro(900));
    CheckMs("0.4 ms of slop with the file 1.9 ms back",
            FileTimeToEpochMs(fileTime, systemNow, fileNow), 999999998);
}

// The same unchanged file, read an hour later: both clocks advanced by the
// same amount, so the gap the bridge measures is untouched and the answer has
// to be identical. The extra 0.3 ms of drift is what a real second sample
// looks like — the system clock sample stays inside its millisecond while the
// file-side gap grows past the next one, so a per-half flooring moves the
// answer by a millisecond and the two reads disagree. Static-file caching
// compares this value across requests: a value that moves between reads of one
// untouched file re-sends content the client already holds.
void TestUnchangedFileReadLaterAnswersTheSame() {
    const SysNow systemNow{Milli(1000000000) + Micro(400)};
    const FileTime fileNow{Milli(4000000000)};
    const FileTime fileTime = fileNow - (Milli(1) + Micro(900));
    const auto anHourLater = std::chrono::hours(1) + Micro(300);
    const long long first = FileTimeToEpochMs(fileTime, systemNow, fileNow);
    const long long again = FileTimeToEpochMs(
        fileTime, systemNow + anHourLater, fileNow + anHourLater);
    CheckMs("one unchanged file read an hour apart", again, first);
}

// Direction: the sample pair is taken 5 ms after the write, and the file clock
// read follows the system clock read by 600 us. The result is floored, never
// rounded up, so it lands below the write instant instead of claiming a time
// the file did not have: an HTTP Last-Modified stamp ahead of the actual write
// makes a client answer "not modified" for content it has never seen. The
// 600 us gap is what subtracts, and it is why the caller reads the system
// clock first.
void TestNeverAheadOfTheWrite() {
    constexpr long long kWriteMs = 1000000000;
    const FileTime fileTime{Milli(kWriteMs)};
    const SysNow systemNow{Milli(kWriteMs + 5) + Micro(300)};
    const FileTime fileNow{Milli(kWriteMs + 5) + Micro(900)};
    const long long got = FileTimeToEpochMs(fileTime, systemNow, fileNow);
    if (got > kWriteMs) {
        ++g_fail;
        std::fprintf(stderr,
                     "FAIL: mtime %lld ms is ahead of the write at %lld ms\n",
                     got, kWriteMs);
    } else {
        ++g_pass;
    }
    CheckMs("write floored by the 600 us sampling gap", got, kWriteMs - 1);
}

// Monotonicity: with the sample pair fixed, a later write never reports an
// earlier time, and a full millisecond of extra age is a full millisecond on
// the output.
void TestMonotoneInFileTime() {
    const SysNow systemNow{Milli(1000000000) + Micro(300)};
    const FileTime fileNow{Milli(4000000000)};
    // The sample sits 0.3 ms past its millisecond, so a file 10 ms old reads
    // as 999999990 and each extra millisecond of age adds exactly one.
    constexpr long long kBaseMs = 999999990;
    long long previous = FileTimeToEpochMs(fileNow - Milli(10), systemNow,
                                           fileNow);
    for (int step = 1; step <= 8; ++step) {
        const FileTime fileTime = fileNow - Milli(10) + Milli(step);
        const long long got = FileTimeToEpochMs(fileTime, systemNow, fileNow);
        if (got < previous) {
            ++g_fail;
            std::fprintf(stderr, "FAIL: file age step %d gave %lld ms, "
                                 "below the previous %lld ms\n",
                         step, got, previous);
        } else {
            ++g_pass;
        }
        const long long want = kBaseMs + step;
        if (got != want) {
            ++g_fail;
            std::fprintf(stderr, "FAIL: file %d ms younger than 10 ms old gave "
                                 "%lld ms, want %lld ms\n", step, got, want);
        } else {
            ++g_pass;
        }
        previous = got;
    }
}

} // namespace

int main() {
    std::fprintf(stderr, "=== File Time Epoch Bridge Tests ===\n");
    TestExactMillisecondScale();
    TestSubMillisecondPartsCancel();
    TestUnchangedFileReadLaterAnswersTheSame();
    TestNeverAheadOfTheWrite();
    TestMonotoneInFileTime();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
