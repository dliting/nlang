#Unit tests for the e2e runner's scratch namespace
#(NLANG_E2E_SCRATCH_TAG), the isolation sharded runs need. The
#fs-writing corpora address their scratch through the literal relative
#path ../temp/fs/..., resolved against the run CWD, so a shard is
#isolated by giving it a CWD inside a tag-owned subtree — not by
#renaming the scratch dir, which the corpora's literals would keep
#pointing at. Paths come from scratch_paths(); the subprocess case
#proves the environment plumbing and that a teardown stays inside its
#own namespace. Launches are sequential: no timing assertions.
import os
import shutil
import subprocess
import sys
import tempfile

import pytest

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

import run_e2e_tests as runner


def test_untagged_scratch_keeps_the_historical_paths(monkeypatch):
    monkeypatch.setattr(runner, 'RUN_CWD', None)
    paths = runner.scratch_paths('')
    legacy = os.path.normpath(os.path.join(runner.SCRIPT_DIR, '..', 'temp'))
    assert paths.ns is None
    assert os.path.normpath(paths.fs) == os.path.join(legacy, 'fs')
    assert os.path.normpath(paths.imports) == os.path.join(legacy, 'imports')
    assert os.path.normpath(paths.examples) == os.path.join(legacy, 'examples')
    assert paths.run_cwd is None
    #Untagged runs keep running the program in tests/e2e, as before.
    assert runner.run_dir_for(runner.SCRIPT_DIR) == runner.SCRIPT_DIR


def test_tagged_scratch_nests_under_a_private_root(monkeypatch):
    paths = runner.scratch_paths('shard_001')
    root = os.path.normpath(
        os.path.join(runner.SCRATCH_ROOT, 'shard_001'))
    assert os.path.normpath(paths.fs) == os.path.join(root, 'temp', 'fs')
    assert os.path.normpath(paths.imports) == os.path.join(root, 'imports')
    assert os.path.normpath(paths.examples) == os.path.join(root, 'examples')
    assert os.path.normpath(paths.run_cwd) == os.path.join(root, 'cwd')
    #The invariant the nesting buys: with the run CWD set to the tag's
    #dir, a corpus's literal ../temp/fs/... lands inside that tag.
    assert os.path.normpath(
        os.path.join(paths.run_cwd, '..', 'temp', 'fs')) == \
        os.path.normpath(paths.fs)
    #The tagged manifest pass runs its program in the tag's dir; the
    #examples pass keeps owning its own out_dir as CWD.
    monkeypatch.setattr(runner, 'RUN_CWD', os.path.normpath(paths.run_cwd))
    assert runner.run_dir_for(runner.SCRIPT_DIR) == \
        os.path.normpath(paths.run_cwd)
    assert runner.run_dir_for(paths.examples) == os.path.normpath(paths.examples)


def test_two_tags_share_no_scratch_path():
    first = runner.scratch_paths('shard_000')
    second = runner.scratch_paths('shard_001')
    for key in ('fs', 'imports', 'examples', 'run_cwd'):
        assert os.path.normpath(getattr(first, key)) != \
            os.path.normpath(getattr(second, key))


@pytest.mark.parametrize('bad_tag', ['a/b', 'a\\b', '..', 'shard 0', '/abs'])
def test_scratch_tag_rejects_path_shaping(bad_tag):
    with pytest.raises(ValueError, match='NLANG_E2E_SCRATCH_TAG'):
        runner.scratch_paths(bad_tag)


def test_reset_clears_only_its_own_dir(tmp_path):
    mine = tmp_path / 'shard_a' / 'temp' / 'fs'
    sibling = tmp_path / 'shard_b' / 'temp' / 'fs'
    mine.mkdir(parents=True)
    (mine / 'leftover.txt').write_text('a', encoding='utf-8')
    sibling.mkdir(parents=True)
    (sibling / 'sibling.txt').write_text('b', encoding='utf-8')

    runner.reset_scratch_dir(str(mine))

    assert mine.is_dir()
    assert not (mine / 'leftover.txt').exists()
    assert (sibling / 'sibling.txt').read_text(encoding='utf-8') == 'b'


#Driver for the subprocess case: materialize this run's scratch, leave a
#marker in it, and optionally tear it down. Imported paths are what the
#environment variable selected, so the case covers the whole chain.
_DRIVER = """
import os
import sys

sys.path.insert(0, sys.argv[1])
import run_e2e_tests as runner

runner.prepare_scratch()
with open(os.path.join(runner.FS_SCRATCH, 'marker.txt'), 'w',
          encoding='utf-8') as handle:
    handle.write('x')
if sys.argv[2] == 'teardown':
    runner.teardown_scratch()
print(runner.SCRATCH_NS)
"""


def _run_tagged_driver(tag, action):
    """Run the driver under one scratch tag and report its namespace root."""
    with tempfile.TemporaryDirectory() as work:
        script = os.path.join(work, 'driver.py')
        with open(script, 'w', encoding='utf-8') as handle:
            handle.write(_DRIVER)
        env = dict(os.environ, NLANG_E2E_SCRATCH_TAG=tag)
        done = subprocess.run(
            [sys.executable, script, SCRIPT_DIR, action],
            capture_output=True, text=True, env=env, timeout=120)
    assert done.returncode == 0, f'{done.stdout}{done.stderr}'
    return os.path.normpath(done.stdout.strip())


def test_teardown_removes_only_its_own_namespace():
    """A shard's teardown must not delete a concurrent shard's files: the
    driver writes a marker under its tag, and a second tagged run tearing
    down must leave the first tag's marker behind."""
    ns_first = _run_tagged_driver('shard_iso_a', 'keep')
    try:
        marker = os.path.join(ns_first, 'temp', 'fs', 'marker.txt')
        assert os.path.isfile(marker)
        ns_second = _run_tagged_driver('shard_iso_b', 'teardown')
        assert not os.path.exists(ns_second)
        assert os.path.isfile(marker)
    finally:
        shutil.rmtree(ns_first, ignore_errors=True)
