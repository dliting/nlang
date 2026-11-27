/*---
    FileTimeEpoch.h — the bridge from a <filesystem> timestamp to Unix epoch
    milliseconds, shared by the fs native module and its unit tests.

    <filesystem>'s clock has an implementation-defined epoch, so a file
    timestamp reaches the Unix epoch only by way of a sample of that same
    clock: "how far in the past the write happened", measured in the file
    clock's own domain, added to the system clock's epoch. The two samples are
    parameters instead of being read here, which keeps the arithmetic testable
    and puts the sampling order with the caller that has to honor it.
---*/
#pragma once

#include <chrono>
#include <filesystem>

namespace nlang::native {

// Converts one file timestamp to milliseconds since the Unix epoch.
// `systemNow` and `fileNow` are one sample pair of the two clocks: each clock
// read once, at the same instant, with the system clock read first so the gap
// between the two reads subtracts instead of pushing the result into the
// future.
//
// The two durations are added at full resolution and floored to milliseconds
// once. Flooring each half on its own instead makes the pair's sub-millisecond
// parts stop cancelling — each is cut in its own direction — so the answer of
// one unchanged file depends on where those parts happen to land and wanders by
// a millisecond between reads, which for a static file's Last-Modified/ETag
// means re-sending content the client already holds. The single floor also
// keeps the estimate at or below the write instant: with the system clock read
// first the sampling gap subtracts, and what is left of its sub-millisecond
// part falls off the end instead of rounding the estimate up.
inline long long FileTimeToEpochMs(std::filesystem::file_time_type fileTime,
                                   std::chrono::system_clock::time_point systemNow,
                                   std::filesystem::file_time_type fileNow) {
    using Milli = std::chrono::milliseconds;
    const auto sinceEpoch = systemNow.time_since_epoch() + (fileTime - fileNow);
    return std::chrono::duration_cast<Milli>(sinceEpoch).count();
}

} // namespace nlang::native
