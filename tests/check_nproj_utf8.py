#!/usr/bin/env python3
"""ctest guard: .nproj project files must be UTF-8.

ncc validates the raw bytes of a project file up front and rejects a
legacy-encoded save with a named UTF-8 error (a UTF-8 BOM is accepted
and skipped; UTF-16 saves get a dedicated hint). The fixtures are
generated at runtime as byte-exact files — GBK bytes come from
Python's codec, not from a binary blob in the repo.
"""
import os
import shutil
import subprocess
import sys

UTF8_BOM = b"\xef\xbb\xbf"

NPROJ_TEMPLATE = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<!-- {comment} -->\n'
    '<Project name="{name}" outputDir="">\n'
    '  <Sources>\n'
    '    <File path="main.n"/>\n'
    '  </Sources>\n'
    '</Project>\n'
)

MAIN_N = (
    'int main() {\n'
    '    return 7;\n'
    '}\n'
)


def run_ncc(ncc, workdir, project_name):
    """Run `ncc build -p <project>.nproj` and capture (exit, stdout, stderr)."""
    project = os.path.join(workdir, project_name)
    proc = subprocess.run(
        [ncc, "build", "-p", project],
        cwd=workdir,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    return (
        proc.returncode,
        proc.stdout.decode("utf-8", errors="replace"),
        proc.stderr.decode("utf-8", errors="replace"),
    )


def write_bytes(workdir, name, payload):
    path = os.path.join(workdir, name)
    with open(path, "wb") as handle:
        handle.write(payload)
    return path


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def main():
    if len(sys.argv) != 3:
        print("usage: check_nproj_utf8.py <ncc> <scratch-dir>")
        return 2
    ncc = os.path.abspath(sys.argv[1])
    scratch = os.path.abspath(sys.argv[2])

    # One project dir per case; each holds its own .nproj + main.n.
    cases = ["gbk", "utf16", "bom_ok"]
    workdirs = {}
    for case in cases:
        workdir = os.path.join(scratch, "nproj_utf8_" + case)
        if os.path.exists(workdir):
            shutil.rmtree(workdir)
        os.makedirs(workdir)
        write_bytes(workdir, "main.n", MAIN_N.encode("utf-8"))
        workdirs[case] = workdir

    # Case 1 — GBK save (the XML declaration claims UTF-8, the bytes lie):
    # a Chinese-locale editor in ANSI mode produces exactly this.
    gbk_proj = NPROJ_TEMPLATE.format(name="工程", comment="note")
    write_bytes(workdirs["gbk"], "gbk.nproj", gbk_proj.encode("gbk"))
    code, out, err = run_ncc(ncc, workdirs["gbk"], "gbk.nproj")
    if code == 0:
        fail("GBK .nproj was accepted (exit 0); expected a named UTF-8 error")
    if "not valid UTF-8" not in (out + err):
        fail("GBK .nproj rejection lacks the 'not valid UTF-8' diagnosis; "
             "stderr: " + err.strip())
    print("case 1 ok: GBK .nproj rejected with a UTF-8 diagnosis")

    # Case 2 — UTF-16LE save (Notepad "Unicode" mode, FF FE BOM): a
    # dedicated hint, not a generic invalid-byte report.
    utf16_proj = NPROJ_TEMPLATE.format(name="proj", comment="note")
    payload = b"\xff\xfe" + utf16_proj.encode("utf-16-le")
    write_bytes(workdirs["utf16"], "utf16.nproj", payload)
    code, out, err = run_ncc(ncc, workdirs["utf16"], "utf16.nproj")
    if code == 0:
        fail("UTF-16 .nproj was accepted (exit 0); expected rejection")
    if "UTF-16" not in (out + err):
        fail("UTF-16 .nproj rejection lacks the dedicated 'UTF-16' hint; "
             "stderr: " + err.strip())
    print("case 2 ok: UTF-16 .nproj rejected with the UTF-16 hint")

    # Case 3 — UTF-8 with BOM (multi-byte content in the XML comment):
    # the BOM is accepted and skipped, the project compiles end-to-end.
    # The project name stays ASCII — module names are ASCII identifiers.
    bom_proj = NPROJ_TEMPLATE.format(name="bomproj", comment="中文注释")
    write_bytes(workdirs["bom_ok"], "bom.nproj",
                UTF8_BOM + bom_proj.encode("utf-8"))
    code, out, err = run_ncc(ncc, workdirs["bom_ok"], "bom.nproj")
    if code != 0:
        fail("UTF-8 BOM .nproj failed to compile; "
             "stdout: " + out.strip() + "; stderr: " + err.strip())
    expected_nmod = os.path.join(workdirs["bom_ok"], "bomproj.nmod")
    if not os.path.exists(expected_nmod):
        fail("compiled .nmod not found at " + expected_nmod)
    print("case 3 ok: UTF-8 BOM .nproj accepted and compiled")

    for case in cases:
        shutil.rmtree(workdirs[case], ignore_errors=True)
    print("check_nproj_utf8: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
