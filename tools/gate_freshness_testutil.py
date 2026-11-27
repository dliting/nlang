"""gate_freshness_testutil.py -- the stub repository and tree builders
and the helpers the freshness self-test corpus shares
(gate_freshness_test.py and its companion case module
gate_freshness_native_test.py). A stub repo carries the inputs the
rules read: three sweep files, one native unit reaching three headers
in the forms the native rule resolves, and one public header no native
unit includes. A stub tree carries the artifacts one tree config
holds. Every file gets a pinned mtime, so a test states its case by
moving one stamp and the corpus stays hermetic: no build, no network.
Stdlib only.
"""
import os
from pathlib import Path

import gate_freshness


REPO_INPUTS = ["src/vm/vm_core.cpp", "include/nlang/abi.h", "stdlib/util.n"]

#The native module's unit reaches its headers in the three forms the
#native rule resolves: the public header through the project's
#<nlang/...> path, a same-dir private header through a quoted name, and
#a helper outside src/native through a relative path.
NATIVE_UNIT = (b'#include <nlang/abi.h>\n'
               b'#include "io_slots.h"\n'
               b'#include "../../runtime/NativeTimers.h"\n')
NATIVE_HEADERS = ["src/native/io/io_slots.h", "src/runtime/NativeTimers.h"]
#A public header no native unit includes: the tools compile it, the
#native DLLs do not.
UNREACHED_HEADER = "include/nlang/compiler/ModuleBuilder.h"

REPO = Path(__file__).resolve().parent.parent
#Quoted includes of the real native units that resolve to neither
#include/ nor the unit's own directory: the vendored zlib, reached
#through its own include directory. Anything else here is a project
#header the native rule would silently judge as a system header.
VENDORED_NATIVE_INCLUDES = {"zlib.h"}


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def write(path, content=b"x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return path


def set_mtime(path, seconds):
    os.utime(str(path), (seconds, seconds))


def make_repo(root):
    for rel in (REPO_INPUTS + NATIVE_HEADERS + [UNREACHED_HEADER]):
        set_mtime(write(root / rel), 100)
    unit = write(root / "src/native/io/io_native.cpp", NATIVE_UNIT)
    set_mtime(unit, 100)
    return root


def make_tree(root, tools=("ncc", "nvm", "ndb"), native=True,
              stdlib=True, tests=False, version=True):
    for tool in tools:
        set_mtime(write(root / "src" / "tools" / tool / "Debug"
                        / ("%s.exe" % tool)), 200)
    if native:
        set_mtime(write(root / "native" / "Debug" / "nlang_io.dll"), 200)
    if stdlib:
        set_mtime(write(root / "stdlib" / "util.n"), 150)
    if version:
        set_mtime(write(root / "generated" / "nlang_version.h"), 150)
    if tests:
        set_mtime(write(root / "tests" / "Debug" / "test_vm.exe"), 200)
    return root


PACKAGE_DOMAIN = "io"


def write_package(tree, domain=PACKAGE_DOMAIN, mtime=250):
    """A generated per-domain project with its compiled package: cmake
    writes the <domain>.nproj at configure time, the build writes the
    package stamped at `mtime` (250 = fresh: newer than the tools at
    200 and the stdlib copy at 150)."""
    set_mtime(write(tree / "stdlib" / ("%s.nproj" % domain)), 150)
    set_mtime(write(tree / "stdlib" / ("%s.npkg" % domain)), mtime)


def write_package_project(tree, domain=PACKAGE_DOMAIN):
    """The generated project alone: a tree configured for the package
    whose packing target has not run since."""
    set_mtime(write(tree / "stdlib" / ("%s.nproj" % domain)), 150)


def run_check(repo, specs):
    return gate_freshness.check(
        repo,
        [gate_freshness.TreeSpec.parse(s) if isinstance(s, str) else s
         for s in specs])


def expect_usage_error(thunk, what):
    try:
        thunk()
    except gate_freshness.UsageError:
        return
    check(False, "expected a UsageError: %s" % what)
