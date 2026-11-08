# 15. The system Package

The `system` package exposes clocks and calendar types —
machinery about machine time, kept apart from data processing
(`io` and the string methods).

## 15.1 Clocks

| Function | Returns | Meaning |
| --- | --- | --- |
| `system.wallMs()` | `long` | Wall clock: Unix epoch milliseconds (the `CLOCK_REALTIME` domain). Subject to NTP steps and manual changes — use it for timestamps. |
| `system.monoMs()` | `long` | Monotonic clock: milliseconds since an arbitrary epoch, never going backwards. The same clock domain as `io.parkForIo` deadlines — use it for intervals, timeouts and timers. |

Measure elapsed time with two `monoMs()` calls, not two
`wallMs()` calls: a clock adjustment makes the wall clock jump
forwards or backwards.

## 15.2 DateTime

`DateTime` is an immutable UTC calendar timestamp on the
proleptic Gregorian calendar; there is no local-timezone
handling yet, and every value is UTC.

```nlang
import io;
import system;

int main() {
    DateTime t = DateTime.parse("2026-01-01T00:00:00.000Z");
    io.print(t.addMillis(500).toString());
    return 0;
}
```

| Member | Meaning |
| --- | --- |
| `year`, `month`, `day`, `hour`, `minute`, `second`, `millis` | Normalized calendar fields (`month` 1..12, `hour` 0..23, `minute`/`second` 0..59, `millis` 0..999). |
| `DateTime.now()` | The current wall-clock instant. |
| `DateTime.fromUnixMillis(long)` | Decompose Unix epoch milliseconds. |
| `DateTime(long)` | Same as `fromUnixMillis`. |
| `toUnixMillis()` | The inverse: Unix epoch milliseconds. |
| `addMillis(long)` | A new `DateTime` shifted by the amount (may cross days, months or years). |
| `dayOfWeek()` | 0 = Sunday .. 6 = Saturday. |
| `toString()` | ISO 8601 UTC form `YYYY-MM-DDThh:mm:ss.mmmZ`. |
| `DateTime.parse(string)` | Parses that form (the fractional part is optional); malformed text raises `DateTimeException`. |

`DateTime.parse` is strict: the separators must match
`toString` exactly, the year must be four digits, and the day
must exist in the given month (February 30 is rejected).
