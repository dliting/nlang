# Installation and Layout

Two distribution forms, identical content:

- **NSIS installer** (`NLang-<version>-win64.exe`): installs to
  `C:\Program Files\NLang` by default and creates Start Menu shortcuts;
- **portable zip** (`NLang-<version>-win64.zip`): unpack and use — the
  directory layout matches the installer's.

Layout of the installation root:

| Directory/file | Contents |
|---|---|
| `bin\` | ncc.exe, nvm.exe, ndisasm.exe, ndb.exe, nide.exe, and the Qt runtime |
| `examples\` | all example programs, including the multi-file project `hello_project` |
| `docs\site\` | this help site — exactly what nide's embedded help window loads |
| `LICENSE`, `README.md` | license and project description |

Two things to note:

- `C:\Program Files` is read-only for regular users, while a build
  writes its `.nmod` next to the project files — before opening an
  example in nide, copy `examples\` to a writable directory.
- The executables link the MSVC runtime dynamically and require the
  [VC++ Redistributable for Visual Studio 2015-2022](https://aka.ms/vs/17/release/vc_redist.x64.exe)
  (machines with Visual Studio 2022 installed usually have it).
