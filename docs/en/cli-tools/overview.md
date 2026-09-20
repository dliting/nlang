# Overview

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
| nide | The graphical IDE (build, run, and debug) | [Three Ways to Run](../getting-started/running.md), [Debugging in nide](../getting-started/debugging.md) |

Each tool page is the complete reference for its invocation forms,
flags, and behavior.

## Version Queries

All four tools report their version with `--version`, in the form
`ncc (NLang) <version>`; nide shows its version in Help → About, and
the documentation site in its footer.
