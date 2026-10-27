"""NLEP archive hygiene: four-digit dense numbering, required header
fields, status from the closed set. Proposals are mutable living
records (no backward compatibility), but their identity fields stay
stable."""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PROPOSALS = REPO / "docs" / "spec" / "proposals"
NAME = re.compile(r"^(\d{4})-.+\.md$")
HEADER = re.compile(r"^# NLEP-(\d{4}): (.+)$", re.M)
FIELDS = ("Status", "Created", "Spec-Sections")
STATUSES = {"Draft", "Accepted", "Rejected", "Superseded"}


def _proposal_pages():
    for p in sorted(PROPOSALS.glob("*.md")):
        if p.name != "TEMPLATE.md" and NAME.match(p.name):
            yield p


def test_file_names_wellformed():
    #_proposal_pages skips non-conforming names; without this check a
    #mis-named proposal (3-digit number, wrong prefix) would vanish
    #from every check and the numbering would still read dense.
    assert PROPOSALS.is_dir(), "the proposal archive must exist"
    for p in PROPOSALS.glob("*.md"):
        if p.name != "TEMPLATE.md":
            assert NAME.match(p.name), \
                f"{p}: file name must be '<4 digits>-<title>.md'"


def test_numbers_dense_unique():
    assert PROPOSALS.is_dir(), "the proposal archive must exist"
    nums = sorted(int(NAME.match(p.name).group(1)) for p in _proposal_pages())
    assert nums == list(range(1, len(nums) + 1)), nums


def test_headers_complete():
    assert PROPOSALS.is_dir(), "the proposal archive must exist"
    for p in _proposal_pages():
        text = p.read_text(encoding="utf-8")
        header = HEADER.search(text)
        assert header, f"{p}: missing '# NLEP-NNNN: Title' header"
        assert header.group(1) == NAME.match(p.name).group(1), \
            f"{p}: header number != file name number"
        for field in FIELDS:
            assert re.search(rf"^\*\*{field}:\*\* \S", text, re.M), \
                f"{p}: missing **{field}:**"
        status = re.search(r"^\*\*Status:\*\* (\w+)", text, re.M).group(1)
        assert status in STATUSES, f"{p}: unknown status {status}"
