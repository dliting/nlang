"""The README pair's example programs are audited like manual pages: the
fences use ```nlang and the audit runs them with the tree's own tools."""
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nlang_docs.snippets import audit_doc  # noqa: E402

NCC = os.environ.get("NLANG_NCC")
NVM = os.environ.get("NLANG_NVM")
needs_tools = pytest.mark.skipif(
    not (NCC and NVM and os.path.isfile(NCC) and os.path.isfile(NVM)),
    reason="ncc/nvm not provided via NLANG_NCC/NLANG_NVM")


@needs_tools
@pytest.mark.parametrize("readme", ["README.md", "README.zh-CN.md"])
def test_readme_example_compiles_and_runs(readme, tmp_path):
    repo = Path(__file__).resolve().parents[3]
    problems, programs, fragments, errors = audit_doc(
        repo / readme, NCC, NVM, tmp_path / "work")
    assert problems == []
    assert len(programs) == 1
    assert fragments == [] and errors == []
