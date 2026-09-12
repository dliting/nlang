"""Public-text guard: the tracked tree and the release package carry no
references to NLang's private predecessor codebase.

The single pattern source is nlang_docs.public_text; this suite pins its
detection behavior (mutations land red) and scans every git-tracked
text file. The release-side twin lives in tests/packaging/verify_package.py.
"""
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nlang_docs import public_text  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]


def test_forbidden_forms_are_detected():
    #One sample per pattern family: zh narrative, en narrative,
    #comment-style lineage refs, predecessor paths and mechanism names.
    samples = [
        "NLang 是一门受 EN 游戏引擎脚本语言启发的语言",
        "inspired by the EN game engine",
        "继承自游戏引擎的执行模型",
        "//Reference: EN's WhileStmt::DoCompile (SeStatements.cpp:468).",
        "(compiler_bak/grammer/nlang.y:535)",
        "原实现在 E:/cases/en 仓库",
        "历史代码在 e:\\CASES\\EN 下",
        "the opcode is patched by FixChainedJumps",
        "emit I_Base_Case + jump placeholder",
    ]
    for text in samples:
        violations = public_text.find_violations(text)
        assert violations, f"mutation not caught: {text!r}"


def test_benign_text_is_not_flagged():
    #Adjacent-but-legal forms: English words containing "en", the VM's
    #"execution engine" sense of 引擎, lowercase locale codes, the
    #predecessor-free present-tense descriptions.
    samples = [
        "translated from English",
        "children of the paragraph node",
        "translations/qtwebengine_locales/en-US.pak",
        "several use cases/energy budgets were reviewed",
        "the VM execution engine walks frames",
        "执行引擎在安全点触发 GC",
        "reference counting was rejected",
    ]
    for text in samples:
        assert not public_text.find_violations(text), \
            f"false positive on: {text!r}"


def _tracked_text_files():
    #git ls-files is the exact public set: local-only files (untracked)
    #never appear, so the gate reads exactly what a clone would.
    try:
        result = subprocess.run(
            ["git", "ls-files"], cwd=REPO_ROOT,
            capture_output=True, timeout=60)
    except OSError as exc:
        pytest.skip(f"tracked-file listing unavailable: {exc}")
    if result.returncode != 0:
        #No git metadata (e.g. an exported tarball): skip loudly rather
        #than silently scanning nothing.
        pytest.skip("git ls-files failed: "
                    + result.stderr.decode("utf-8", "replace")[:200])
    return result.stdout.decode("utf-8").splitlines()


def test_tracked_tree_has_no_forbidden_text():
    #The guard's own two files define the patterns and pin them with
    #mutation samples — they necessarily contain the forbidden strings,
    #so the scan must not flag its own source.
    self_exempt = {
        Path("tools/nlang-docs/src/nlang_docs/public_text.py"),
        Path("tools/nlang-docs/tests/test_public_text.py"),
    }
    tracked = _tracked_text_files()
    scanned = 0
    for rel in tracked:
        if Path(rel) in self_exempt:
            continue
        path = REPO_ROOT / rel
        if not path.is_file():
            continue  #stale index entry
        raw = path.read_bytes()
        if b"\0" in raw[:8192]:
            continue  #binary (image, .ico, .nmod fixture)
        scanned += 1
        text = raw.decode("utf-8", errors="replace")
        violations = public_text.find_violations(text)
        assert not violations, \
            f"forbidden public text in {rel}: {violations[:3]}"
    #A scan that read nothing is a wiring bug, not a pass.
    assert scanned > 100, f"suspiciously few files scanned: {scanned}"
