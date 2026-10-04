#!/usr/bin/env python3
"""ctest guard: the CLI tools must handle full-Unicode (UTF-8) paths.

The tool executables declare a UTF-8 active code page (manifest), so
argv arrives as UTF-8, narrow file APIs accept it, and the tools' own
diagnostics echo paths as UTF-8 bytes — the same console contract
io.print already documents. Without it, the CRT narrows wide argv
through the system code page: CJK path segments round-trip through it
but echo back in that code page (mojibake for UTF-8 readers like nide),
and segments outside it (emoji, rare CJK) are destroyed into '?' before
main() even starts.

Usage: python check_cli_utf8_paths.py <ncc> <nvm> <ndisasm> <ndb> <work-dir>

Layers pinned, one temp tree with a mixed CJK+emoji directory name:
  1. ncc build <dir>/prog.n -o <dir>/prog.ncu — argv ingestion (the
     emoji would be '?'-replaced without the UTF-8 code page), the
     .ncu lands where asked, and stdout decodes as strict UTF-8 with
     the directory name intact.
  2. nvm <dir>/prog.ncu — runs the module from the non-ASCII path;
     the program writes and reads back a non-ASCII file name (the fs/io
     narrow-path conversion shares the process code page), exit code
     passthrough, stdout strict UTF-8 with the content intact.
  3. ndisasm <dir>/prog.ncu — reads the module, stdout strict UTF-8
     with the directory name intact (func.sourceFile carries it).
  4. ndb <dir>/prog.ncu driven with "c" — same argv + fs/io pipeline
     through the debugger entry.
  5. failure diagnostics: nvm on a missing module under the non-ASCII
     path must fail with strict-UTF-8 stderr echoing the name (the
     channel nide's output pages decode).
"""
import os
import shutil
import subprocess
import sys

TIMEOUT_SEC = 60
EXPECTED_EXIT = 3
EXIT_CONTENT_MISMATCH = 11

#The directory name mixes a CJK segment (representable in legacy code
#pages — would only mojibake) and an emoji (unrepresentable — would be
#destroyed into '?'), so both failure layers are covered.
NON_ASCII_DIR = "目录中文😀"
NON_ASCII_FILE = "文件😀.txt"
FILE_CONTENT = "内容"
SOURCE = (
    "import io;\n"
    "\n"
    "int main() {\n"
    '    io.writeFile("%s", "%s");\n'
    '    string s = io.readFile("%s");\n'
    "    if (s != \"%s\") {\n"
    "        return %d;\n"
    "    }\n"
    "    io.print(s);\n"
    "    return %d;\n"
    "}\n" % (NON_ASCII_FILE, FILE_CONTENT, NON_ASCII_FILE,
             FILE_CONTENT, EXIT_CONTENT_MISMATCH, EXPECTED_EXIT)
)


def run(argv, stdin_bytes=None):
    return subprocess.run(argv, capture_output=True,
                          input=stdin_bytes, timeout=TIMEOUT_SEC)


def fail(msg):
    print("FAIL " + msg)
    return 1


def utf8_or_fail(raw, channel):
    """Decode strict UTF-8; (text, 0) on success, (None, 1) on failure."""
    try:
        return raw.decode("utf-8"), 0
    except UnicodeDecodeError as e:
        return None, fail("%s is not UTF-8 (code-page echo): %s" % (channel, e))


def main():
    ncc, nvm, ndisasm, ndb, work_dir = sys.argv[1:6]
    pin_dir = os.path.join(work_dir, "cli_utf8_pin")
    if os.path.isdir(pin_dir):
        shutil.rmtree(pin_dir)
    tree = os.path.join(pin_dir, NON_ASCII_DIR)
    os.makedirs(tree)
    src = os.path.join(tree, "prog.n")
    with open(src, "w", encoding="utf-8", newline="\n") as f:
        f.write(SOURCE)
    ncu = os.path.join(tree, "prog.ncu")

    #Layer 1: compile — argv in, echoed path out.
    r = run([ncc, "build", src, "-o", ncu])
    if r.returncode != 0:
        return fail("ncc build exited %d: %r" % (r.returncode, r.stderr[:200]))
    if not os.path.isfile(ncu):
        return fail("ncc did not produce the .ncu at the non-ASCII path")
    out, err_code = utf8_or_fail(r.stdout, "ncc stdout")
    if out is None:
        return err_code
    if NON_ASCII_DIR not in out:
        return fail("ncc stdout lost the non-ASCII directory name: %r" % out)

    #Layer 2: run — the program round-trips a non-ASCII file name via
    #fs/io (both go through the process code page) and prints the
    #content back.
    r = run([nvm, ncu])
    if r.returncode != EXPECTED_EXIT:
        return fail("nvm exited %d, want %d: %r" % (
            r.returncode, EXPECTED_EXIT, r.stderr[:200]))
    out, err_code = utf8_or_fail(r.stdout, "nvm stdout")
    if out is None:
        return err_code
    if FILE_CONTENT not in out:
        return fail("nvm stdout lost the round-tripped content: %r" % out)
    if not os.path.isfile(NON_ASCII_FILE):
        return fail("the program did not create the non-ASCII file name")
    os.remove(NON_ASCII_FILE)

    #Layer 3: disassemble.
    r = run([ndisasm, ncu])
    if r.returncode != 0:
        return fail("ndisasm exited %d: %r" % (r.returncode, r.stderr[:200]))
    out, err_code = utf8_or_fail(r.stdout, "ndisasm stdout")
    if out is None:
        return err_code
    if NON_ASCII_DIR not in out:
        return fail("ndisasm stdout lost the non-ASCII directory name")

    #Layer 4: the debugger entry speaks the same argv + fs/io pipeline.
    r = run([ndb, ncu], stdin_bytes=b"c\n")
    if r.returncode != EXPECTED_EXIT:
        return fail("ndb exited %d, want %d: %r" % (
            r.returncode, EXPECTED_EXIT, r.stderr[:200]))
    out, err_code = utf8_or_fail(r.stdout, "ndb stdout")
    if out is None:
        return err_code
    if FILE_CONTENT not in out:
        return fail("ndb stdout lost the round-tripped content: %r" % out)
    if os.path.isfile(NON_ASCII_FILE):
        os.remove(NON_ASCII_FILE)

    #Layer 5: failure diagnostics echo the non-ASCII path as UTF-8 on
    #stderr (the channel nide decodes).
    missing = os.path.join(tree, "缺模块.ncu")
    r = run([nvm, missing])
    if r.returncode == 0:
        return fail("nvm unexpectedly succeeded on a missing module")
    err, err_code = utf8_or_fail(r.stderr, "nvm stderr")
    if err is None:
        return err_code
    if NON_ASCII_DIR not in err:
        return fail("nvm stderr lost the non-ASCII directory name: %r" % err[:200])

    shutil.rmtree(pin_dir)
    print("PASS CLI tools handle UTF-8 paths end to end")
    return 0


if __name__ == "__main__":
    sys.exit(main())
