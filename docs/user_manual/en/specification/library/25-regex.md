# 25. The regex Package

The `regex` package searches, splits, and replaces text with regular
expressions. The engine simulates an automaton instead of backtracking,
so the worst-case running time of every pattern is linear in the input
— a pattern from untrusted sources cannot blow up the program. The
price of that guarantee is a boundary on the syntax: back references
and look-around assertions are the constructs whose known
implementations need backtracking, so they are not supported, and the
engine says so at compile time instead of quietly reading them as
plain text (§25.4).

Two classes carry the work. `Regex` compiles a pattern once and is
from then on an immutable value — reusable for any number of inputs
and shareable across concurrent contexts. `Match` is the read-only
result of one match. Matching is a substring search, the same posture
as `indexOf`, not a whole-string comparison: an unanchored pattern
finds its earliest occurrence anywhere inside the input, and
`\A...\z` around a pattern is how a whole-string match is written
(§25.5). The package is pure NLang — no native code.

## 25.1 A tour

```nlang
// A tour of the regex package: named capture groups, repeated
// matches, and replace templates.
import io;
import regex;

int main() {
    Regex re = Regex("(?P<key>\\w+)=(?P<value>[^\\s]*)");
    Match m = re.match("server=127.0.0.1 port=8080");
    if (m != null) {
        io.print("key=" + m.group("key") + " value=" + m.group("value"));
    }
    List<Match> all = re.matches("server=127.0.0.1 port=8080");
    io.print("count=" + all.length().toString());
    for (int i = 0; i < all.length(); i = i + 1) {
        Match pair = all[i];
        io.print("at " + pair.start.toString() + ": " + pair.text);
    }
    // $$ is the dollar escape: the template layer sees ${value}; and
    // expands the value group for every match.
    io.print(re.replaceAll("a=1 b=2", "$${value};"));
    io.print("ok");
    return 0;
}
```

This fence is the shipped `examples/regex_tour.n`, and it prints:

```text
key=server value=127.0.0.1
count=2
at 0: server=127.0.0.1
at 17: port=8080
1; 2;
ok
```

The first line shows a named group read back from the match; the
`at` lines show that offsets are byte offsets into the input; the
`1; 2;` line shows a template expansion over both matches — the
space between them belongs to no match and passes through
untouched. §25.7 explains why the source writes `\\w` and
`$${value};` where the pattern and the template really say `\w` and
`${value};`.

## 25.2 The Regex class

| Member | Meaning |
| --- | --- |
| `Regex(pattern)` | Compiles the pattern. A failure raises `RegexException`; the message names the reason, and most messages also give a byte offset in the pattern (§25.4). |
| `pattern()` | The pattern text as given to the constructor. |
| `isMatch(input)` | Whether the input has a match; the cheapest test when no content is wanted. |
| `match(input)` | The leftmost match as a `Match`, or `null` when there is none. No match is a normal result, not an error (§4.3). |
| `matches(input, limit = -1)` | Every non-overlapping match, left to right, as a `List<Match>`. `limit < 0` (the default) means no limit; `limit >= 0` caps the count. Empty matches follow the skip rule of §25.9. |
| `split(input, limit = -1)` | The segments between matches (§25.9). |
| `replaceAll(input, template)` | Replaces every match through the template (§25.8). |
| `replaceFirst(input, template)` | Replaces only the leftmost match. |

`Regex` is a `const class`: the constructor compiles, and nothing
after that mutates the object, so one `Regex` serves any number of
contexts running concurrently (each match keeps its working state to
itself). `RegexException` derives from `IOException` — as
`UrlException` and `HttpException` do — because patterns commonly
come from configuration files and captured text from I/O, and one
`catch (IOException e)` covers bad patterns together with read
failures. The free function `regex.quoteMeta(s)` escapes every
metacharacter of its argument so that the result matches that text
literally — `quoteMeta("a.b*c")` gives `a\.b\*c` — the tool for
splicing untrusted text into a pattern.

## 25.3 The Match class

A `Match` is a read-only value describing one successful match.

| Member | Meaning |
| --- | --- |
| `text` | The matched text. |
| `start` / `end` | The match's byte offsets in the input; `end` is exclusive, the same convention as `substring` (§4.3). |
| `groupCount` | The number of capture groups, group 0 (the whole match) included. |
| `group(index)` | The text of that capture group. A group that did not participate in the match gives `""`; an out-of-range index raises `RegexException`. |
| `group(name)` | The same by group name; an unknown name raises `RegexException` — a misspelled name is a bug, and silence here would bury it. |
| `groupStart(index)` / `groupEnd(index)` | The byte offsets of a group; `-1` when the group did not participate. |

Group 0 is the whole match: `group(0)` equals `text`, and
`groupStart(0)` / `groupEnd(0)` equal `start` / `end`.

## 25.4 Pattern syntax

| Construct | Form |
| --- | --- |
| Literal | `abc` — text in any language; literals match whole code points (§25.6). |
| Any character | `.` — one code point; excludes `\n` unless `(?s)` is on. |
| Alternation and groups | `a|b`, `(re)` captures, `(?:re)` groups without capturing, `(?P<name>re)` and `(?<name>re)` name a capturing group; two groups sharing a name are a compile error. |
| Repetition | `*` `+` `?` `{n}` `{n,}` `{n,m}` — greedy by default; a trailing `?` prefers fewer repetitions. A repetition count above 1000 is rejected. |
| Flags | `(?i)` case-insensitive matching, `(?m)` multi-line anchors, `(?s)` the dot matches newline. `(?flags)` takes effect from there to the end of the enclosing group, `(?flags:re)` applies only to `re`, and `-` clears a flag — `(?i-s:re)` sets `i` and clears `s`. |
| Anchors | `^ $ \A \z \b \B` (§25.5). |
| Escapes | `\a \f \t \n \r \v` and escaped punctuation; `\123` octal — one to three octal digits: a first digit `0` makes an octal escape on its own (`\0`), and a non-zero first digit counts as octal only when another octal digit follows it (`\41`, `\101`; a lone `\1`–`\7` stays the back-reference form, rejected below); `\xHH`, `\x{10FFFF}`, and `\Q...\E` to quote a stretch of pattern text literally. |
| Classes | `[abc]`, negation `[^abc]`, ranges `[a-z]`, POSIX classes such as `[[:alpha:]]`, and `\d \D \s \S \w \W` — all American Standard Code for Information Interchange (ASCII) only (§25.6). Inside a class, escapes work and `\b` means the backspace character U+0008, the reading Perl-family engines share; `&` and `~` are ordinary class members there. |

The engine rejects, with its own `RegexException`, every construct
that would break the linear-time guarantee or whose meaning mainstream
engages split on: back references (`\1`), look-around (`(?=) (?!`
`(?<=) (?<!)`), Unicode property classes (`\p{...}`), `\Z`, the
global greed swap `(?U)`, the extended layout `(?x)`, possessive
quantifiers (`*+`, `++`), atomic groups (`(?>re)`), and class set
operations — `[a&&b]` compiles, but as the plain member set `a`, `&`
and `b`. Each rejection names its reason, and most carry a byte
offset in the pattern, for example
`regex: back references are not supported at byte 1 of pattern`.
Three size guards — on pattern nesting depth, compiled program size
and repetition counts — keep a hostile pattern from exhausting
memory, each with its own message.

## 25.5 Anchors and line boundaries

- `^` — the start of the input; with `(?m)`, also the start of each
  line.
- `$` — the end of the input, or the point just before a final
  `\n`; with `(?m)`, also the end of each line. `Regex("abc$").isMatch("abc\n")`
  is true.
- `\A` and `\z` — exactly the start and the end of the input,
  unaffected by `(?m)`. `\z` is the strict end for when `$`'s
  courtesy to a trailing newline would be wrong; `\A...\z` together
  pins the whole string.
- `\b` and `\B` — word boundary and non-boundary, computed over the
  ASCII word set (§25.6).

## 25.6 Code points and the ASCII domain

Matching reads the input as Unicode Transformation Format (UTF-8)
code points: `.` matches one code point of any text, and a literal
matches as whole code points, never as byte fragments. Bytes that
form no valid UTF-8 sequence each decode to one U+FFFD replacement
character (the Go rule): every invalid byte claims one match slot one
byte wide, `\x{FFFD}` hits them one by one, and an invalid byte is
never read as its own code point — a lone `0xA9` byte does not match
the literal `©`. Matching neither fails on invalid input nor rewrites
it. Reported offsets are byte offsets, the same basis as `substring`
and `indexOf` (§4.3).

Letters, digits and spaces, in contrast, mean ASCII: the class
shorthands `\d \s \w`
and their negations, the POSIX classes, `(?i)` case folding and the
`\b` word set cover ASCII only. Both halves of the rule are positive
rules, not footnotes:

```nlang
Regex("a.b").isMatch("a中b")       // true  — one code point, any script
Regex("\\w+").isMatch("汉字")      // false — the ASCII word set
Regex("(?i)cafe").isMatch("café")  // false — folding is ASCII letters only
Regex("(?i)cafe").isMatch("cafe")  // true
```

## 25.7 Patterns and templates in NLang source

Pattern text and templates are ordinary string literals, and two
characters meet the language's own literal rules on the way:

- A pattern's backslash must be written doubled, because the string
  literal consumes backslashes first: the pattern `\d+` is written
  `"\\d+"`. NLang has no raw-string form, and a lone `\d` in source
  is an unknown string escape — a compile error.
- A template's `$` meets the `${name}` string interpolation: write
  `$${name}` and the literal layer delivers `${name}` to the template
  engine. To put a literal `$` in the replacement text, write
  `"$$$"` — the literal layer folds it to the template's `$$`, and
  the template emits `$`.

```nlang
import io;
import regex;

int main() {
    Regex g = Regex("(\\w)(\\d)");
    io.print(g.replaceAll("x7", "$1x $${2}y") + "\n");  // " 7y" — see §25.8
    io.print(g.replaceFirst("x7", "$${1}x") + "\n");    // "xx"
    io.print(Regex("a+").replaceFirst("a-aa-a", "$$$") + "\n");  // "$-aa-a"
    return 0;
}
```

## 25.8 Replacement templates

In a template passed to `replaceAll` / `replaceFirst`:

- `$$` produces a literal `$`.
- `$name` and `${name}` expand a capture group. The run of letters,
  digits and underscores immediately after `$` is read as one name,
  greedily: `$1x` refers to a group *named* `1x`, not group `1`
  followed by `x`; writing a number and appending text therefore
  takes `${1}x`.
- A name that is all digits with no leading zeros is a group number;
  group 0 is the whole match. `$10` means group 10. Unmatched text
  passes through untouched: pattern `a+` and template `[$0]` turn
  `a-aa-a` into `[a]-[aa]-[a]`.
- A group that does not exist, or that did not participate in this
  match, expands to the empty string.
- A `$` that forms no reference — followed by no name character and
  no second `$` — is left as a literal `$`: `$!` stays `$!`; a `${`
  that never closes forms no reference either, and everything from
  there stays verbatim, so `${1` stays `${1`.

## 25.9 split and empty matches

`split(input, limit = -1)` cuts the input at every match:
`limit < 0` (the default) keeps every segment, `limit == 0` returns
an empty list, and `limit > 0` returns at most that many segments,
the last one holding the remainder. Captured groups never become
segments — the answer is the text between matches, which is what
makes the `limit` counting sensible.

```nlang
Regex(",").split("a,b,,c", -1)    // ["a", "b", "", "c"]
Regex(",").split("a,b,,c", 2)     // ["a", "b,,c"]
Regex(",").split("a,b,,c", 0)     // []
Regex("a*").split("ab", -1)       // ["", "b"]
```

Patterns that can match nothing need one adjacency rule, shared by
`split` and `matches`: an empty match starting exactly where the
previous match ended is skipped — without the skip, `a*` would stall
forever at the empty position behind the `a` it just consumed —
while an empty match anywhere else, including the end of the input,
still counts. So `Regex("a*").matches("ab")` returns two matches,
`a` at 0 and the empty one at 2.

`split` then cuts the input at those matches with two segment
rules: a zero-width match at text position 0 contributes no leading
empty segment, and the final remainder is appended only when the
last separator does not start at the text end. That is why the
`split` example above gives `["", "b"]` — the `a` match, being
non-empty, leaves the leading empty segment, while the empty match
at 2 starts at the text end and leaves no trailing one — and why
`Regex("x*").split("abc")` gives `["a", "b", "c"]`: each empty
match contributes the text before it, and neither text end
contributes an empty edge segment.
