> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 15. system包

`system`包暴露时钟与日历类型——关于机器时间的设施，与数据处理（`io`与字符串方法）分开放置。

## 15.1 时钟

| 函数 | 返回 | 含义 |
| --- | --- | --- |
| `system.wallMs()` | `long` | 墙钟：Unix纪元毫秒（`CLOCK_REALTIME`域）。受NTP步进与手动调整影响——用它做时间戳。 |
| `system.monoMs()` | `long` | 单调钟：自某个任意起点的毫秒数，永不倒退。与`io.parkForIo`的截止期同一时钟域——用它测间隔、做超时与定时器。 |

测耗时用两次`monoMs()`，不要用两次`wallMs()`：时钟调整会让墙钟跳进或跳回。

## 15.2 DateTime

`DateTime`是proleptic Gregorian历法上的不可变UTC日历时刻，暂无本地时区处理；所有值都是UTC。

```nlang
import io;
import system;

int main() {
    DateTime t = DateTime.parse("2026-01-01T00:00:00.000Z");
    io.print(t.addMillis(500).toString());
    return 0;
}
```

| 成员 | 含义 |
| --- | --- |
| `year`、`month`、`day`、`hour`、`minute`、`second`、`millis` | 规范化日历字段（`month` 1..12、`hour` 0..23、`minute`/`second` 0..59、`millis` 0..999）。 |
| `DateTime.now()` | 当前墙钟时刻。 |
| `DateTime.fromUnixMillis(long)` | 分解Unix纪元毫秒。 |
| `DateTime(long)` | 同`fromUnixMillis`。 |
| `toUnixMillis()` | 逆向：Unix纪元毫秒。 |
| `addMillis(long)` | 平移后的新`DateTime`（可跨日、月、年）。 |
| `dayOfWeek()` | 0 = 周日 .. 6 = 周六。 |
| `toString()` | ISO 8601 UTC形式`YYYY-MM-DDThh:mm:ss.mmmZ`。 |
| `DateTime.parse(string)` | 解析该形式（小数部分可有可无）；畸形输入抛`DateTimeException`。 |

`DateTime.parse`是严格的：分隔符必须与`toString`完全一致，年份必须四位数字，日期必须在所给月份内存在（2月30日被拒绝）。
