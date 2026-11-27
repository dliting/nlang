#!/usr/bin/env python3
"""End-to-end package-artifact import corpus.

CTest passes real ncc and nvm executables. Every source and generated package
lives below one isolated scratch directory; no package is written beside the
e2e corpus, where it could shadow unrelated tests.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

COMMAND_TIMEOUT_SECONDS = 60


class CorpusFailure(RuntimeError):
    pass


def normalized_path(path):
    return str(Path(path).resolve()).replace('\\', '/')


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')


def write_project(root, name, sources, natives=()):
    source_rows = ''.join(
        f'    <File path="{source}"/>\n' for source in sources)
    native_rows = ''.join(
        f'    <Native path="{native}"/>\n' for native in natives)
    natives_block = (
        '  <Natives>\n'
        f'{native_rows}'
        '  </Natives>\n') if natives else ''
    write_text(
        root / f'{name}.nproj',
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<Project name="{name}">\n'
        '  <Sources>\n'
        f'{source_rows}'
        '  </Sources>\n'
        f'{natives_block}'
        '</Project>\n')
    return root / f'{name}.nproj'


def invoke(args, expected=0, env_extra=None):
    environment = None
    if env_extra:
        #Explicit overrides on top of the inherited environment: the
        #corpus must never fall back to the real per-user cache, even
        #when it is driven directly instead of through ctest.
        environment = dict(os.environ)
        environment.update(env_extra)
    result = subprocess.run(
        [normalized_path(arg) if isinstance(arg, Path) else str(arg)
         for arg in args],
        capture_output=True,
        text=True,
        timeout=COMMAND_TIMEOUT_SECONDS,
        env=environment)
    if result.returncode != expected:
        raise CorpusFailure(
            f"command returned {result.returncode}, expected {expected}:\n"
            f"  {' '.join(str(arg) for arg in args)}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}")
    return result


def build_package(ncc, project, output, import_dirs=()):
    output.parent.mkdir(parents=True, exist_ok=True)
    args = [ncc, 'build', '-p', project]
    for directory in import_dirs:
        args.extend(['-I', directory])
    args.extend(['-o', output])
    invoke(args)


def run_package(nvm, package, import_dirs=()):
    args = [nvm, package]
    for directory in import_dirs:
        args.extend(['-I', directory])
    return invoke(args)


def make_library(root, name, sources):
    for relative_path, source in sources.items():
        write_text(root / relative_path, source)
    return write_project(root, name, sources.keys())


def make_app(root, name, source):
    write_text(root / 'main.n', source)
    return write_project(root, name, ['main.n'])


def test_artifact_surface(ncc, nvm, root):
    library_sources = {
        'core.n': 'int base() { return 5; }\n',
        'pkg/sub.n': 'int child() { return 7; }\n',
        'leaf.n': 'int value() { return 11; }\n',
        'api.n': (
            'import leaf;\n'
            'int total() { return leaf.value(); }\n'),
    }
    library_root = root / 'surface' / 'library_source'
    project = make_library(
        library_root, 'surface_lib', library_sources)
    package_dir = root / 'surface' / 'packages'
    library_package = package_dir / 'surface_lib.npkg'
    build_package(ncc, project, library_package)

    app_root = root / 'surface' / 'app'
    app = make_app(
        app_root,
        'surface_app',
        'import io;\n'
        'import core;\n'
        'import pkg.*;\n'
        'import api;\n'
        'int main() {\n'
        '    int answer = core.base() + pkg.sub.child() + api.total();\n'
        '    if (answer != 23) { return 9; }\n'
        '    io.print(answer);\n'
        '    return 0;\n'
        '}\n')
    app_package = root / 'surface' / 'output' / 'surface_app.npkg'
    build_package(ncc, app, app_package, [package_dir])
    result = run_package(nvm, app_package, [package_dir])
    if result.stdout.strip() != '23':
        raise CorpusFailure(
            f'artifact surface printed {result.stdout!r}, expected 23')


def test_source_precedes_earlier_artifact(ncc, nvm, root):
    stale_root = root / 'dual_order' / 'stale_source'
    stale_project = make_library(
        stale_root, 'choice_stale',
        {'choice.n': 'int value() { return 1; }\n'})
    artifact_first = root / 'dual_order' / 'artifact_first'
    build_package(
        ncc, stale_project, artifact_first / 'choice_stale.npkg')

    source_later = root / 'dual_order' / 'source_later'
    write_text(
        source_later / 'choice.n',
        'string value() { return "source"; }\n')
    #Runtime loading has its own directory-order rule. Keep the source
    #implementation as a loose unit and put that directory first at run time;
    #the compile step below deliberately uses the opposite order to pin D2.
    invoke([
        ncc, 'build', source_later / 'choice.n',
        '-o', source_later / 'choice.ncu'])
    app_root = root / 'dual_order' / 'app'
    app = make_app(
        app_root,
        'dual_order_app',
        'import io;\n'
        'import choice;\n'
        'int main() {\n'
        '    string answer = choice.value();\n'
        '    if (answer != "source") { return 9; }\n'
        '    io.print(answer);\n'
        '    return 0;\n'
        '}\n')
    app_package = root / 'dual_order' / 'output' / 'dual_order_app.npkg'
    build_package(ncc, app, app_package, [artifact_first, source_later])
    result = run_package(nvm, app_package, [source_later, artifact_first])
    if result.stdout.strip() != 'source':
        raise CorpusFailure(
            f'later source did not outrank earlier artifact: {result.stdout!r}')


def test_source_artifact_parity(ncc, nvm, root):
    source_dir = root / 'parity' / 'source'
    project = make_library(
        source_dir, 'parity_lib',
        {'parity.n': 'int value() { return 13; }\n'})
    artifact_dir = root / 'parity' / 'artifact'
    build_package(ncc, project, artifact_dir / 'parity_lib.npkg')
    invoke([
        ncc, 'build', source_dir / 'parity.n',
        '-o', source_dir / 'parity.ncu'])

    good_root = root / 'parity' / 'good_app'
    good_project = make_app(
        good_root,
        'parity_app',
        'import io;\n'
        'import parity;\n'
        'int main() { io.print(parity.value()); return 0; }\n')
    source_app = root / 'parity' / 'output' / 'source_app.npkg'
    artifact_app = root / 'parity' / 'output' / 'artifact_app.npkg'
    build_package(ncc, good_project, source_app, [source_dir])
    build_package(ncc, good_project, artifact_app, [artifact_dir])
    source_run = run_package(nvm, source_app, [source_dir])
    artifact_run = run_package(nvm, artifact_app, [artifact_dir])
    if source_run.stdout != artifact_run.stdout or source_run.stdout.strip() != '13':
        raise CorpusFailure(
            'source/artifact behavior differs:\n'
            f'source={source_run.stdout!r}\nartifact={artifact_run.stdout!r}')

    bad_root = root / 'parity' / 'bad_app'
    bad_project = make_app(
        bad_root,
        'parity_bad',
        'import parity;\n'
        'int main() { return parity.missing(); }\n')
    source_diag = invoke(
        [ncc, 'build', '-p', bad_project, '-I', source_dir,
         '-o', root / 'parity' / 'output' / 'bad_source.npkg'],
        expected=1)
    artifact_diag = invoke(
        [ncc, 'build', '-p', bad_project, '-I', artifact_dir,
         '-o', root / 'parity' / 'output' / 'bad_artifact.npkg'],
        expected=1)
    source_text = source_diag.stdout + source_diag.stderr
    artifact_text = artifact_diag.stdout + artifact_diag.stderr
    if source_text != artifact_text:
        raise CorpusFailure(
            'source/artifact diagnostics differ:\n'
            f'source:\n{source_text}\nartifact:\n{artifact_text}')


def test_first_artifact_directory_wins(ncc, nvm, root):
    first_source = root / 'directory_order' / 'first_source'
    first_project = make_library(
        first_source, 'choice_first',
        {'choice.n': 'int value() { return 31; }\n'})
    second_source = root / 'directory_order' / 'second_source'
    second_project = make_library(
        second_source, 'choice_second',
        {'choice.n': 'int value() { return 47; }\n'})
    first_dir = root / 'directory_order' / 'first_packages'
    second_dir = root / 'directory_order' / 'second_packages'
    build_package(ncc, first_project, first_dir / 'choice_first.npkg')
    build_package(ncc, second_project, second_dir / 'choice_second.npkg')

    app_root = root / 'directory_order' / 'app'
    app = make_app(
        app_root,
        'directory_order_app',
        'import io;\n'
        'import choice;\n'
        'int main() {\n'
        '    int answer = choice.value();\n'
        '    if (answer != 31) { return 9; }\n'
        '    io.print(answer);\n'
        '    return 0;\n'
        '}\n')
    app_package = root / 'directory_order' / 'output' / 'app.npkg'
    build_package(ncc, app, app_package, [first_dir, second_dir])
    result = run_package(nvm, app_package, [first_dir, second_dir])
    if result.stdout.strip() != '31':
        raise CorpusFailure(
            f'first artifact directory did not win: {result.stdout!r}')


def test_native_member_cache(ncc, nvm, pkgprobe_dll, root):
    case_root = root / 'native_member_cache'

    # Library: pkgprobe declares the natives, plain is a plain NLang
    # unit, and the probe library rides the archive as a native member.
    # The member keeps the host spelling the loader resolves
    # (nlang_pkgprobe.dll on Windows, libnlang_pkgprobe.so on POSIX —
    # the hardcoded .dll name once shipped a member no POSIX loader
    # could ever find, and the run silently answered from the build
    # tree's copy instead). The directory copy is deleted after
    # packing, so every run below must find the cache copy — no loose
    # neighbor can answer.
    probe_name = pkgprobe_dll.name
    library_root = case_root / 'library'
    write_text(
        library_root / 'pkgprobe.n',
        '// Declarations for the package-native probe; the bodies live\n'
        '// in the probe library file, which this package ships as a\n'
        '// native archive member.\n'
        'native int value();\n'
        'native string where();\n')
    write_text(
        library_root / 'plain.n',
        'int triple(int x) {\n'
        '    return x * 3;\n'
        '}\n')
    shutil.copy(pkgprobe_dll, library_root / probe_name)
    library_project = write_project(
        library_root, 'probelib', ['pkgprobe.n', 'plain.n'],
        [probe_name])
    package_dir = case_root / 'packages'
    library_package = package_dir / 'probelib.npkg'
    build_package(ncc, library_project, library_package)
    (library_root / probe_name).unlink()

    marks_root = case_root / 'marks'
    runs_root = case_root / 'runs'
    #The attach mark is written by the DLL itself (kernel32 file calls
    #from DllMain cannot create directories); the corpus owns the mark
    #directory, the runtime owns the cache directories.
    marks_root.mkdir(parents=True, exist_ok=True)
    runs_root.mkdir(parents=True, exist_ok=True)

    def run_program(nvm_program, cache_root, mark_path):
        return invoke(
            [nvm, nvm_program, '-I', package_dir],
            env_extra={
                'NLANG_PKG_CACHE_ROOT': str(cache_root),
                'PKGPROBE_ATTACH_MARK': str(mark_path),
            })

    # Arm one — load and call: the caller imports the declaration unit
    # and calls the natives. where() must answer a path under the
    # cache root (the packed copy answered, not some shadow elsewhere)
    # and the attach mark must exist (the module mapped in).
    caller_app = make_app(
        case_root / 'app', 'probe_caller',
        'import io;\n'
        'import pkgprobe;\n'
        'int main() {\n'
        '    io.print(pkgprobe.value());\n'
        '    io.print(pkgprobe.where());\n'
        '    return 0;\n'
        '}\n')
    caller_package = case_root / 'output' / 'probe_caller.npkg'
    build_package(ncc, caller_app, caller_package, [package_dir])
    cache_caller = runs_root / 'cache_caller'
    result = run_program(caller_package, cache_caller,
                         marks_root / 'attach_caller.mark')
    lines = result.stdout.splitlines()
    cache_text = normalized_path(cache_caller)
    if len(lines) < 2 or lines[0] != '7341':
        raise CorpusFailure(
            f'native call through the cache printed {result.stdout!r}, '
            'expected 7341 then a path')
    if (not lines[1].replace('\\', '/').startswith(cache_text + '/')
            or Path(lines[1]).name != probe_name):
        raise CorpusFailure(
            f'where() answered outside the package cache: {lines[1]!r}')
    if not (marks_root / 'attach_caller.mark').exists():
        raise CorpusFailure(
            'attach mark missing: the probe library never mapped into '
            'the calling process')

    # Arm two — extracted, not loaded: this program imports only the
    # plain unit, so the package's native declarations never enter the
    # closure and the probe must stay out of the process (mark absent)
    # even though opening the package extracted its native member to
    # the cache (cache copy present).
    plain_app = make_app(
        case_root / 'app_plain', 'probe_plain',
        'import io;\n'
        'import plain;\n'
        'int main() { io.print(plain.triple(3)); return 0; }\n')
    plain_package = case_root / 'output' / 'probe_plain.npkg'
    build_package(ncc, plain_app, plain_package, [package_dir])
    cache_plain = runs_root / 'cache_plain'
    mark_plain = marks_root / 'attach_plain.mark'
    result = run_program(plain_package, cache_plain, mark_plain)
    if result.stdout.strip() != '9':
        raise CorpusFailure(
            f'plain unit through the package printed {result.stdout!r}, '
            'expected 9')
    if mark_plain.exists():
        raise CorpusFailure(
            'attach mark present: the probe library mapped in although '
            'its declarations are outside the closure')
    if not list(cache_plain.glob(f'*/{probe_name}')):
        raise CorpusFailure(
            'cache copy missing: opening the package must extract its '
            'native members even when no arm of the program loads them')

    # Arm three — re-extraction on corruption: a corrupted cache copy
    # must not survive; the next open re-extracts it and the run
    # behaves as if nothing happened.
    cached = list(cache_caller.glob(f'*/{probe_name}'))
    if len(cached) != 1:
        raise CorpusFailure(
            f'expected exactly one cached probe copy, found {len(cached)}')
    cached[0].write_bytes(b'corrupted')
    result = run_program(caller_package, cache_caller,
                         marks_root / 'attach_caller_again.mark')
    lines = result.stdout.splitlines()
    if (len(lines) < 2 or lines[0] != '7341'
            or not lines[1].replace('\\', '/').startswith(cache_text + '/')):
        raise CorpusFailure(
            f'run after corrupting the cached copy printed '
            f'{result.stdout!r}: the re-extraction did not restore it')
    if not (marks_root / 'attach_caller_again.mark').exists():
        raise CorpusFailure(
            'attach mark missing after re-extraction: the restored copy '
            'never mapped into the calling process')


def main():
    if len(sys.argv) != 5:
        print(f'usage: {Path(sys.argv[0]).name} <ncc> <nvm> '
              f'<pkgprobe-dll> <scratch-root>')
        return 2
    ncc = normalized_path(sys.argv[1])
    nvm = normalized_path(sys.argv[2])
    pkgprobe_dll = Path(sys.argv[3]).resolve()
    scratch_root = Path(sys.argv[4]).resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(
                prefix='pkg_artifact_imports_', dir=scratch_root) as temp:
            root = Path(temp)
            test_artifact_surface(ncc, nvm, root)
            test_source_precedes_earlier_artifact(ncc, nvm, root)
            test_source_artifact_parity(ncc, nvm, root)
            test_first_artifact_directory_wins(ncc, nvm, root)
            test_native_member_cache(ncc, nvm, pkgprobe_dll, root)
    except (CorpusFailure, subprocess.TimeoutExpired) as error:
        print(f'pkg_artifact_imports FAILED:\n{error}')
        return 1
    print('pkg_artifact_imports passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
