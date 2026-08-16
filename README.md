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
   `flex.exe` and `bison.exe` in a known directory (e.g. `D:\dev\win_flex_bison\`)
3. Configure with explicit Flex/Bison paths:
   ```bash
   cmake -B build -DFLEX_EXE="D:/dev/win_flex_bison/flex.exe" \
                  -DBISON_EXE="D:/dev/win_flex_bison/bison.exe"
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
| `-DNLANG_BUILD_EXAMPLES` | OFF | Build example programs |
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

## Project Structure

```
include/nlang/runtime/     - Runtime public headers
include/nlang/compiler/    - Compiler public headers
include/nlang/vm/          - VM public headers (.nmod format constants)
src/runtime/               - Runtime implementation
src/compiler/              - Compiler implementation (grammar, generated, builder)
src/vm/                    - VM backend implementation
src/tools/ncc/             - Command-line compiler
src/tools/ndisasm/         - Bytecode disassembler
src/tools/nide/            - Qt5 IDE
```

## License

MIT
