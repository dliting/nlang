#!/usr/bin/env python3
"""ctest guard: a tool's --version must print exactly
`<tool> (NLang) <version>` (configure-time VERSION value) and exit 0.

Usage: python check_version.py <tool-path> <tool-name> <expected-version>
"""
import subprocess
import sys

TIMEOUT_SEC = 60


def main():
    tool_path, tool_name, expected = sys.argv[1], sys.argv[2], sys.argv[3]
    want = "%s (NLang) %s" % (tool_name, expected)
    r = subprocess.run([tool_path, "--version"], capture_output=True,
                       timeout=TIMEOUT_SEC)
    got = r.stdout.decode("utf-8", "replace").strip()
    if r.returncode != 0:
        print("FAIL %s --version exited %d" % (tool_name, r.returncode))
        return 1
    if got != want:
        print("FAIL %s --version printed %r, want %r" % (tool_name, got, want))
        return 1
    print("PASS %s --version = %s" % (tool_name, got))
    return 0


if __name__ == "__main__":
    sys.exit(main())
