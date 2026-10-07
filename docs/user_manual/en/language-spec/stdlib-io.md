# io — content IO

`io` carries all content reads and writes: the
console and disk text files. Multi-byte input decodes as
Unicode Transformation Format (UTF-8) into full code points
(`readChar`); disk content round-trips as bytes.

| Function | Signature | Notes |
|----------|-----------|-------|
| write | (string\|array\|scalar primitive) → void | stdout, **no** newline, flush |
| eprint | (string\|array\|scalar primitive) → void | stderr + '\n' + flush |
| print | (string\|array\|scalar primitive) → void | stdout + '\n' + flush |
| readLine | () → string | stdin line, trailing '\r' stripped |
| hasInput | () → bool | true while input remains (never raises). Exact for file/pipe redirection; a console reports only buffered input; a debug session only parked lines |
| readToken | () → string | skips whitespace (across lines) for the next non-whitespace token; no token at end of input raises IOException |
| readChar | () → char | next non-whitespace character as a full code point (UTF-8 decoded); raises IOException at end of input, Exception on invalid sequences |
| typed readers | readInt()/readLong()/readFloat()/readDouble()/readBool() | one readToken() token handed to the matching string method |
| readFile | (string) → string | whole file as bytes; failure → IOException |
| writeFile | (string path, string s) → void | create/truncate; failure → IOException |
| appendFile | (string path, string s) → void | create/append; failure → IOException |

`write`, `print` and `eprint` share one argument policy (the coercing trio
above): `write("Name: ")` emits a prompt without a newline so a typed reply
lands on the same console line; `eprint` mirrors `print` on stderr for
diagnostics that stay separable from normal output. Inside a debug session
the two streams merge into the session's single output view.

**input prompt on an interactive console**: when the program is about to
block on an input read and both stdin and stdout are consoles, the runtime
prints a `"> "` prompt before waiting — the REPL convention (Python,
sqlite3), for both a direct `nvm` run and ncc's in-process run. Whenever
either end is a pipe or a redirection (scripted input, `nvm x > out.txt`)
both streams stay byte-exact; the nide debug terminal draws the same token
itself in line mode.

**end of file (EOF) semantics of readLine**: EOF and an empty input line both
return `""` (same as C++ `std::getline`); the empty-string sentinel cannot
tell them apart — that is `hasInput()`'s job (see the next section). Each of
the coercing trio (`write`/`print`/`eprint`) takes exactly one argument;
print several values with several calls.

`readFile` enforces a 16 MiB cap (the same bound the deserializer applies to
untrusted length prefixes); larger files raise IOException.

### Token reads and mixed semantics

Besides line reads, io offers token reads: `readToken()` skips whitespace
(space, tab, carriage return, newline — across lines) and returns the next
token; `readChar()` returns the next non-whitespace character as a full code
point (UTF-8 decoded). On top of them, `readInt()`, `readLong()`,
`readFloat()`, `readDouble()` and `readBool()` take one token and hand it to
the matching string method, failing exactly like a direct call to it.

End-of-input behavior aligns with C/C++/Java: line-level consumption
(`readLine`) treats it as a normal end and returns an empty string; demanding
a value (`readToken`, `readChar` and the five typed readers) at end of input
raises IOException. The empty-string sentinel cannot tell an empty line from
end of input — that is `hasInput()`'s job (never raises):

```nlang
while (io.hasInput()) {
    string line = io.readLine();
    io.print(line);
}
```

Mixed reads follow the C++ `cin >>`/`getline` combination: a token read
consumes up to the token's end, and the following `readLine()` returns the
remainder of that line (leading whitespace included); only when the line is
exhausted does it take the next one. For input `"10 20\n30\n"`: `readInt()`
yields 10, `readInt()` yields 20, `readLine()` yields `""` (empty
remainder), `readLine()` yields `"30"`, and the next read returns `""` (end
of input).

`readChar()` returns literal characters: input x yields 'x'. This is
deliberately different from string.toChar's numeric parse (`"120"` yields
'x'). A byte sequence that is not valid UTF-8 raises Exception.
