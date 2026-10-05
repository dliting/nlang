# Overview

The command-line tools cover the whole pipeline — compile, run, debug,
and disassemble — and fit scripted builds, automated testing, and
headless environments; for a graphical workflow see nide
([Three Ways to Run](../getting-started/running.md)).

Five tools ship in the installation's `bin\` directory, four of them
command-line tools. If PATH is not set up, prefix commands with
`bin\` (e.g. `bin\ncc build ...`).

<!-- This chapter and the Command-line Tools section of README.md are two outlets of the same facts: when the command surface changes, both must be updated; depth may differ. -->

| Tool | Role | Reference |
|---|---|---|
| ncc | Compile (with optional immediate execution) | [ncc](ncc.md) |
| nvm | Run a compiled module | [nvm](nvm.md) |
| ndb | Debug a compiled module | [ndb](ndb.md) |
| ndisasm | Bytecode disassembly | [ndisasm](ndisasm.md) |
| nide | The graphical integrated development environment (IDE) (build, run, and debug) | [Three Ways to Run](../getting-started/running.md), [Debugging in nide](../getting-started/debugging.md) |

Each tool page is the complete reference for its invocation forms,
flags, and behavior.

## Version Queries

All four tools report their version with `--version`, in the form
`ncc (NLang) <version>`; nide shows its version in Help → About, and
the documentation site in its footer.

## Encoding Conventions

The four command-line tools use Unicode Transformation Format (UTF-8) as the process encoding
(declared in the tools' embedded manifest, Windows 10 1903+):
command-line arguments and file paths accept full Unicode,
diagnostics are UTF-8 bytes, and at startup the attached console is
switched to the UTF-8 code page so the default console displays
non-American Standard Code for Information Interchange (ASCII) text. Output redirected to a file or pipe is not altered.

Inputs are UTF-8 too: `.n` source files and `.nproj` project files
are validated before tokenizing — invalid bytes are rejected with an
error (`not valid UTF-8 (first invalid byte at line N)`), a
UTF-16 save gets a dedicated hint, and a leading UTF-8 byte order mark (BOM) is
accepted and skipped.
