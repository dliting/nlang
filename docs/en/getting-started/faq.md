# FAQ

### Where did the `.nmod` go?

`ncc build hello.n` without `-o` writes the module file to the
**current working directory**, not next to the source file — think of
this first when the build output is nowhere to be found. The default
rules and precedence for every form are on the
[ncc](../cli-tools/ncc.md) page.

On the nide side the location follows the output precedence (see
[Three Ways to Run](running.md#configuring-nide)):

- Projects: the `.nproj`'s `outputDir` > nide's global build output
  directory > the project directory;
- Standalone `.n` files: the global build output directory > the
  default location (a `nlang-nide` subdirectory of the user's temp
  directory).

### Exit code not what you expected?

The process exit code is `main`'s return value, and Windows preserves
the full 32-bit value; but POSIX shells (bash, Git-Bash, CI bash steps)
keep only the low 8 bits by convention — `return 300` is seen as 300 in
Python/cmd/PowerShell and as 44 in bash (300 mod 256). The testing
convention is expected values in 0–255 so every observer sees the same
thing.

See also: [Language Specification / Exit Code Convention](../language-spec/exit-code-convention.md).

### Garbled output in the console?

Program output is UTF-8 bytes. A Windows console's default code page
(e.g. GBK on Chinese-locale systems) renders them as mojibake; run
`chcp 65001` first to switch the console to the UTF-8 code page, then
run the program. Note that `fs` and `io` file paths and file names go
through the system active code page — non-ASCII file names do not
necessarily round-trip as UTF-8.

See also: [Language Specification / Standard Library](../language-spec/standard-library.md).

### Where are the docs and search?

The nide Help menu entries NLang Getting Started, Language
Specification, and VM Architecture all open in the IDE's embedded help
window (its content is the documentation site under the installation's
`docs\site\`), with the navigation tree on the left. The search box is
in the window's top-left corner (next to the site title) and supports
full-text search.
