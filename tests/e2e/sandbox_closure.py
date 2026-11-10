#!/usr/bin/env python3
"""Sandbox staging for instrumented executables.

The build tree provisions every instrumented executable's directory with
the MSVC sanitizer dynamic runtime (the root CMakeLists sweep copies
clang_rt*.dll beside each linked exe). A test harness that stages an
executable into a sandbox (temp dir, work dir) MUST carry that closure —
the staged copy dies at load time (0xc0000135) without it, since the
sandbox has no other search hit for the DLL.

stage_executable_closure is the single mechanism for this: it copies the
executable and every clang_rt*.dll sibling beside the SOURCE exe. On
non-sanitizer trees no sibling exists and only the exe is copied, so the
call site stays identical across build flavors. Any harness about to
shutil.copy an .exe (or an instrumented fixture DLL) into a sandbox goes
through here instead — the class rule is "data files travel alone,
executables travel with their runtime closure".
"""

import os
import shutil


def stage_executable_closure(src_exe, dst_dir, dst_name=None):
    """Copy src_exe into dst_dir (name: dst_name or the source basename),
    followed by the sanitizer runtime DLLs provisioned beside the source.
    Returns the staged executable's path."""
    dst_exe = os.path.join(dst_dir,
                           dst_name or os.path.basename(src_exe))
    shutil.copyfile(src_exe, dst_exe)
    src_dir = os.path.dirname(src_exe)
    for name in os.listdir(src_dir):
        if name.startswith('clang_rt') and name.endswith('.dll'):
            shutil.copyfile(os.path.join(src_dir, name),
                            os.path.join(dst_dir, name))
    return dst_exe
