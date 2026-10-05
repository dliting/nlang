# Integrating NLang

The official integration surface today is subprocess driving: your program invokes `ncc` to compile and `nvm` to execute, and talks to the script through the standard streams. This page is for integrators: which files to carry, how to deploy, how to drive and how to debug.

A minimal target program used throughout this page:

```nlang
import io;

int main()
{
    io.print("hello from nlang");
    return 0;
}
```

## 1. Integration shapes

| Shape | Status | Notes |
|---|---|---|
| Subprocess driving | supported today | the host spawns `ncc`/`nvm` and communicates via exit codes and standard streams |
| In-process host application programming interface (API) | not provided today | linking the VM as a C/C++ library |

## 2. What to ship

Based on the install layout in [Installation and Layout](../getting-started/install-layout.md):

| Location | Contents | Needed by |
|---|---|---|
| `bin\nvm.exe` | the VM runner | running programs |
| `bin\ncc.exe` | the compiler | compiling from source |
| `bin\ndb.exe` | the debugger | debug integration |
| `bin\ndisasm.exe` | the bytecode disassembler | deep diagnostics |
| `bin\nide.exe` and the Qt runtime | the integrated development environment (IDE) and its dependencies | GUI development only |
| `bin\nlang_{io,math,fs}.dll` | native implementations of the standard library | programs using the matching namespace |
| `stdlib\` | the standard library | any of io/math/fs |
| `docs\site\` | the manual site | optional |

For run-only deployments `ncc` can be left out; the Qt runtime is needed only by `nide`.

## 3. A minimal runtime set

The minimum is `nvm.exe`, `stdlib\stdlib.npkg` and `nlang_io.dll`, plus the program's own `.ncu`. Place `nlang_io.dll` next to `nvm.exe` and `stdlib.npkg` inside a `stdlib\` subdirectory, then `nvm <program>.ncu` runs. The `.n` declaration sources under `stdlib\` are only used by `ncc` at compile time — at run time only the library package is loaded; bring the matching `nlang_*.dll` for each namespace the program uses. The executables link the MSVC runtime dynamically, so the target machine needs the VC++ 2015-2022 runtime (see [Installation and Layout](../getting-started/install-layout.md)).

## 4. Deployment recipe

- Recommended layout: ship a full NLang install directory next to your application and reference it by absolute path or the `NLANG_PATH` environment variable.
- Project builds write the `.npkg` next to the project file: do not put user projects under read-only locations such as `Program Files` — the `.npkg` write fails.
- Search-path troubleshooting: `--verbose` (or `-v`) prints the resolved import search path per layer, see [ncc](../cli-tools/ncc.md) and [nvm](../cli-tools/nvm.md); `-I` adds a directory.

## 5. Subprocess details

- Exit codes: the program's `main` return value passes through as a 32-bit process exit code, so the host can judge results directly; conventions in [Exit Code Convention](../language-spec/exit-code-convention.md).
- Standard input: write lines to `nvm`'s stdin to drive `io.readline` interactions; program output is Unicode Transformation Format (UTF-8) bytes on stdout, `io.eprint` goes to stderr.
- Compile and run can be one `ncc <file>` invocation, or split into `ncc build` plus `nvm`.

## 6. Debug integration

Unattended debugging uses the `ndb --machine` line protocol: protocol events (including the `error` event) go to stdout, while stderr carries only crash reports and the `--verbose` search-path listing; the host parses line by line to drive breakpoints and stepping. Details in [Debugging](../getting-started/debugging.md) and [ndb](../cli-tools/ndb.md).

## 7. Outlook

Linking the VM in-process as a C/C++ library is future work; until it lands, integration goes through subprocesses.
