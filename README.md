# NLang

NLang is a scripting language compiler and IDE, extracted from the EN game engine
as a standalone teaching/research open-source project.

## Build Dependencies

| Dependency | Version | Required | Notes |
|------------|---------|----------|-------|
| CMake | 3.16+ | Yes | Build system |
| C++ Compiler | C++17 | Yes | MSVC 19.44+ / GCC 9+ / Clang 10+ |
| Flex | 2.6+ | Yes (dev) | Lexer generator. Windows: [win_flex_bison](https://github.com/lexxmark/winflexbison) |
| Bison | 3.0+ | Yes (dev) | Parser generator. Windows: [win_flex_bison](https://github.com/lexxmark/winflexbison) |
| Python3 | 3.13+ | Optional | Build scripts. Conda `py313` env recommended on Windows |
| Qt5 | 5.15+ | IDE only | Core, Xml, Widgets modules |
| LLVM | 15+ | Optional | Code generation backend (`-DNLANG_ENABLE_LLVM=ON`) |

### Windows Development Setup

1. Install Visual Studio 2022 with C++17 support
2. Download [win_flex_bison](https://github.com/lexxmark/winflexbison) and place
   `flex.exe` and `bison.exe` in a known directory (e.g. `C:\dev\win_flex_bison\`)
3. Configure with explicit Flex/Bison paths:
   ```bash
   cmake -B build -DFLEX_EXE="C:/dev/win_flex_bison/flex.exe" \
                  -DBISON_EXE="C:/dev/win_flex_bison/bison.exe"
   ```
4. Python3 is optional (only used for build scripts). If needed, override with
   `-DPYTHON3_EXECUTABLE=<path>`

## Building

```bash
cmake -B build
cmake --build build
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `-DNLANG_ENABLE_LLVM` | OFF | Enable LLVM code generation backend |
| `-DNLANG_BUILD_IDE` | OFF | Build the Qt5 IDE (nide) |
| `-DNLANG_BUILD_TESTS` | OFF | Build unit tests |
| `-DFLEX_EXE` | auto | Path to flex executable |
| `-DBISON_EXE` | auto | Path to bison executable |
| `-DPYTHON3_EXECUTABLE` | auto | Python3 path for build scripts |

### Known Flex/Bison Issues on Windows

- **`unistd.h` inclusion**: Flex 2.6 `%option nounistd` does not prevent
  `<unistd.h>` inclusion in generated header files. We handle this with
  `YY_NO_UNISTD_H` compile definition in CMakeLists.txt.
- **`INT8_MIN` macro redefinition**: Flex generates `INT8_MIN/MAX` macros
  in a non-C99 fallback branch. MSVC's `<stdint.h>` also defines them,
  causing C4005 warnings. We handle this with `%top{ #include <stdint.h> }`
  in `nlang.l`, which includes `<stdint.h>` before flex's definitions,
  so the `#ifndef` guards skip the conflicting macros.

## Command-line Tools

```text
ncc <source.n> [-o out.nmod] [-I <dir>...]        Compile and execute
ncc build <source.n> [-o out.nmod] [-I <dir>...]  Compile only
ncc -p <project.nproj> [-o out.nmod] [-I ...]     Compile and execute a project
ncc build -p <project.nproj> [-o out.nmod]        Compile a project
ncc run <module.nmod>                             Execute only
```

Multi-source projects are described by a `.nproj` XML file (see
`examples/hello_project/`):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Project name="hello_project" namespace="hello_project">
  <Sources>
    <File path="main.n"/>
    <File path="utils.n"/>
  </Sources>
</Project>
```

`name` is the output module name (defaults to the file stem), `outputDir`
optionally redirects the `.nmod` (relative to the project file), and `File`
paths are relative to the project file's directory.

## Standard Library (Phase 11)

`math`, `io` and `fs` are built-in namespaces — reserved names, called
qualified, no import needed. Strings carry built-in methods:

```n
int main() {
    io.print(math.sqrt(2.0));                     // 1.41421
    string s = "hello world".substring(0, 5);     // "hello" (byte offsets)
    List<string> words = "a,b,c".split(",");      // 3 elements
    fs.makeDirs("out");
    io.writeFile(fs.join("out", "greet.txt"), s);
    if (!fs.exists("out/greet.txt")) return 1;
    return words.length();
}
```

Full details — parameter type policy, exception mapping, byte semantics,
deterministic PRNG — are in the Standard Library chapter of
`docs/language-spec.md`; runnable copies of the example live in
`examples/stdlib_*.n`.

## IDE (nide)

```bash
cmake -B build-ide -DNLANG_BUILD_IDE=ON \
    -DCMAKE_PREFIX_PATH=<path-to-qt5.15>
cmake --build build-ide --config Release
```

The build output in `build-ide/src/tools/nide/Release/` is a runnable
layout: the Qt runtime DLLs, the `platforms/` plugin directory, and the
`ncc`/`nvm` tools are copied next to `nide.exe` (the IDE invokes them
from its own directory), so that folder can be copied elsewhere as-is.
The UI language follows the system locale (Chinese and English are
bundled; untranslated strings fall back to their authored text).

Files created through 文件 → 新建文件 join the selected project in the
solution tree (or the solution's sole project); files rename in place
via F2 or the tree/tab context menus (an open dirty editor is saved to
the old path first), and the splitter layout persists across sessions.

`ctest -C Release -R nide_deploy_check` verifies the self-containment: it copies
the layout to a scratch directory, pins Qt's search paths to it via
`qt.conf`, and runs the IDE test suite from there.

## Packaging (Windows)

Release packages are produced with CPack from the IDE build tree (it contains
all tools; a tools-only tree cannot ship `nide`):

```bash
cmake -B build-ide -DNLANG_BUILD_IDE=ON \
    -DFLEX_EXE=<flex> -DBISON_EXE=<bison> \
    -DCMAKE_PREFIX_PATH=<path-to-qt5.15> \
    [-DNLANG_NSIS_MAKENSIS=C:/path/to/makensis.exe]
cmake --build build-ide --config Release
cd build-ide && cpack -C Release -B ../release
```

This produces `release/NLang-<version>-win64.zip` (portable) and
`release/NLang-<version>-win64.exe` (NSIS installer; requires NSIS 3.03+ —
either on `PATH` or passed via `-DNLANG_NSIS_MAKENSIS`). Both contain the
same layout: `bin/` with `nide`, `ncc`, `nvm`, `ndisasm` and the Qt runtime,
plus `examples/`, `docs/`, `LICENSE` and `README.md`. The installer defaults
to `C:\Program Files\NLang` and adds a Start Menu shortcut for the IDE.

Notes:

- The installer defaults to `C:\Program Files\NLang`, which standard users
  cannot write to. Copy `examples/` to a writable folder before opening them
  in the IDE — a build writes its `.nmod` next to the project file.
- The executables link the MSVC runtime dynamically; targets need the
  [VC++ Redistributable for Visual Studio](https://aka.ms/vs/17/release/vc_redist.x64.exe)
  (already present on machines with Visual Studio 2022).
- `python tests/packaging/verify_package.py` checks a built package:
  extracts the zip, asserts the layout, and smoke-tests the packaged
  toolchain by compiling and running `examples/hello.n` with it.

## Project Structure

```
include/nlang/runtime/     - Runtime public headers
include/nlang/compiler/    - Compiler public headers
include/nlang/vm/          - VM public headers (.nmod format constants)
src/runtime/               - Runtime implementation
src/compiler/              - Compiler implementation (grammar, generated, builder)
src/vm/                    - VM backend implementation
src/tools/ncc/             - Command-line compiler
src/tools/nvm/             - VM runner
src/tools/ndisasm/         - Bytecode disassembler
src/tools/nide/            - Qt5 IDE
src/3rdparty/tinyxml2/     - Vendored tinyxml2 10.1.0 (.nproj parsing)
```

## License

MIT
