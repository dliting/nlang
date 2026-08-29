#!/usr/bin/env python3
"""Verifies a built NLang Windows release package (zip + NSIS installer).

Locates the newest NLang-*-win64.zip under release/, extracts it to a
temporary directory, asserts the install layout (binaries, Qt runtime,
examples, docs site, license), then smoke-tests the packaged toolchain by
compiling and running examples/hello.n with the packaged ncc/nvm.
Also asserts the NSIS installer (.exe) exists and is non-empty.

This is a release-step verifier, intentionally not registered in ctest
(packaging is a publishing step, not a per-commit gate).

Usage: python verify_package.py [release_dir]
"""

import glob
import os
import subprocess
import sys
import tempfile
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, '..', '..'))
DEFAULT_RELEASE_DIR = os.path.join(REPO_ROOT, 'release')

# examples/hello.n: public int main() { return 42; }
SMOKE_EXIT_CODE = 42
TIMEOUT_SEC = 60

BIN_FILES = [
    'nide.exe', 'ncc.exe', 'nvm.exe', 'ndisasm.exe',
    'Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll',
    # QtWebEngine runtime for the embedded help browser (the closure
    # defined beside find_package(Qt5) in the root CMakeLists).
    'Qt5WebEngineWidgets.dll', 'Qt5WebEngineCore.dll',
    'Qt5WebChannel.dll', 'Qt5Qml.dll', 'Qt5QmlModels.dll',
    'Qt5Quick.dll', 'Qt5QuickWidgets.dll', 'Qt5Network.dll',
    'Qt5Positioning.dll', 'Qt5PrintSupport.dll',
    'QtWebEngineProcess.exe',
    'resources/icudtl.dat', 'resources/qtwebengine_resources.pak',
    'resources/qtwebengine_resources_100p.pak',
    'resources/qtwebengine_resources_200p.pak',
    'translations/qtwebengine_locales/en-US.pak',
    'translations/qtwebengine_locales/zh-CN.pak',
    'platforms/qwindows.dll',
]
ROOT_FILES = ['LICENSE', 'README.md']
# The mkdocs-generated site ships (the nide Help menu shows it in the
# embedded viewer from <prefix>/docs/site); the two-column stylesheet
# is part of that site.
DOC_FILES = [
    'docs/site/index.html',
    'docs/site/nlang-getting-started.html',
    'docs/site/language-spec/overview.html',
    'docs/site/language-spec/standard-library.html',
    'docs/site/vm-architecture/overview.html',
    'docs/site/search/search_index.json',
    'docs/site/stylesheets/two-column-layout.css',
]
# Markdown sources and internal dev-process docs stay out of the public
# package (only the rendered site ships; roadmap and ci_design reference
# spec files the package doesn't include); Qt debug plugin variants must
# not ship either (release Qt has separate d-suffixed dlls). The devtools
# pak and non-UI locales are over-deployment canaries: the WebEngine
# runtime ships as an exact whitelist (en-US/zh-CN only).
ABSENT_PATHS = [
    #Markdown sources stay out (only the rendered site ships); the split
    #sources live in docs/language-spec/ and docs/vm-architecture/.
    'docs/nlang-getting-started.md', 'docs/language-spec',
    'docs/vm-architecture',
    'docs/superpowers', 'docs/roadmap.md', 'docs/ci_design.md',
    'docs/nide-file-rename-and-layout.md', 'bin/platforms/qwindowsd.dll',
    'bin/resources/qtwebengine_devtools_resources.pak',
    'bin/translations/qtwebengine_locales/fr.pak',
]


def fail(msg):
    print(f'FAIL {msg}')
    sys.exit(1)


def main():
    release_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_RELEASE_DIR

    # --- Locate artifacts -------------------------------------------------
    zips = sorted(glob.glob(os.path.join(release_dir, 'NLang-*-win64.zip')),
                  key=os.path.getmtime)
    if not zips:
        fail(f'no NLang-*-win64.zip found under {release_dir} '
             f'(build packages first: cpack -C Release -B <release_dir>)')
    zip_path = zips[-1]
    print(f'package: {os.path.basename(zip_path)}')

    # The installer shares the zip's file stem — derive it instead of
    # globbing separately, so a stale exe from an older build is never
    # paired with the current zip.
    installer = os.path.splitext(zip_path)[0] + '.exe'
    if not os.path.isfile(installer):
        fail(f'NSIS installer for {os.path.basename(zip_path)} not found: '
             f'expected {installer}')
    if os.path.getsize(installer) == 0:
        fail(f'NSIS installer is empty: {installer}')
    print(f'installer: {os.path.basename(installer)} '
          f'({os.path.getsize(installer) // 1024} KiB)')

    # --- Extract and assert layout ----------------------------------------
    with tempfile.TemporaryDirectory(prefix='nlang_pkg_') as tmp:
        with zipfile.ZipFile(zip_path) as zf:
            names = zf.namelist()
            tops = {n.split('/')[0] for n in names if '/' in n}
            if len(tops) != 1:
                fail(f'expected exactly one top-level dir in zip, got: {tops}')
            pkg_name = tops.pop()
            zf.extractall(tmp)
        pkg = os.path.join(tmp, pkg_name)

        # BIN_FILES are relative to bin/, the rest to the package root.
        for rel in BIN_FILES:
            path = os.path.join(pkg, 'bin', rel.replace('/', os.sep))
            if not os.path.isfile(path):
                fail(f'missing from package bin/: {rel}')
        for rel in ROOT_FILES + DOC_FILES:
            path = os.path.join(pkg, rel.replace('/', os.sep))
            if not os.path.isfile(path):
                fail(f'missing from package: {rel}')
        for rel in ABSENT_PATHS:
            path = os.path.join(pkg, rel.replace('/', os.sep))
            if os.path.exists(path):
                fail(f'internal path shipped in package: {rel}')
        print(f'layout: OK ({len(BIN_FILES)} bin files, '
              f'{len(ROOT_FILES)} root files, {len(DOC_FILES)} site files)')

        # --- Toolchain smoke: compile + run examples/hello.n --------------
        smoke_cwd = os.path.join(tmp, 'smoke')
        os.makedirs(smoke_cwd)
        bin_dir = os.path.join(pkg, 'bin')
        ncc = os.path.join(bin_dir, 'ncc.exe')
        nvm = os.path.join(bin_dir, 'nvm.exe')
        hello = os.path.join(pkg, 'examples', 'hello.n')
        nmod = os.path.join(smoke_cwd, 'hello.nmod')

        r = subprocess.run([ncc, 'build', hello, '-o', nmod],
                           capture_output=True, timeout=TIMEOUT_SEC,
                           cwd=smoke_cwd)
        if r.returncode != 0:
            fail(f'ncc build failed: {r.stderr.decode("utf-8", "replace")[:500]}')
        if not os.path.isfile(nmod):
            fail('ncc reported success but .nmod was not written')

        r = subprocess.run([nvm, nmod], capture_output=True,
                           timeout=TIMEOUT_SEC, cwd=smoke_cwd)
        if r.returncode != SMOKE_EXIT_CODE:
            fail(f'nvm returned {r.returncode}, expected {SMOKE_EXIT_CODE}: '
                 f'{r.stderr.decode("utf-8", "replace")[:500]}')
        print('smoke: OK (packaged ncc compiled and nvm ran hello.n)')

    print('PASS')


if __name__ == '__main__':
    main()
