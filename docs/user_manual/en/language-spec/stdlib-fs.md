# fs — names, directories, metadata

`fs` never reads or writes content — content belongs to `io`. The split is an
operation principle: io = all content (console + disk text), fs = names/directories/metadata.

| Function | Signature | Notes |
|----------|-----------|-------|
| exists / isFile / isDirectory | (string) → bool | never raise |
| size | (string) → int | byte size; regular files only |
| listFiles | (string) → List\<string\> | names only, non-recursive, regular files only, lexicographically sorted |
| makeDirs | (string) → void | mkdir -p (idempotent) |
| remove | (string) → void | file or empty directory |
| join | (string a, string b) → string | `(fs::path(a) / b).generic_string()` — forward slashes everywhere |

**Error model**: `size`/`listFiles`/`makeDirs`/`remove` raise `IOException` on
failure. The three predicates never raise — a path that cannot be stated
(missing, or inaccessible) simply answers `false`. `remove` on a missing path
is a silent no-op. `size` on a directory or special file raises IOException
(directory "sizes" are filesystem noise).

**join edge semantics** (std::filesystem path append, same as Python
`os.path.join`): a rooted right side (`"/b"`) **replaces** the left side; an
empty right side leaves a trailing separator (`join("a","")` is `"a/"`); an
empty left side yields the right side alone.

**Windows encoding note**: paths and filenames convert through the
process active code page (`generic_string`, file opens). The
command-line tools run with Unicode Transformation Format (UTF-8) as the active code page (declared
in the tools' embedded manifest, Windows 10 1903+), so non-American Standard Code for Information Interchange (ASCII)
paths and filenames round-trip as UTF-8 —
`io.readFile`/`writeFile`/`appendFile` included. Embedding hosts
that run the VM under the system code page are still bound by that
code page.
