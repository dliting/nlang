#Unit tests for the e2e runner's stale-artifact refusal
#(_fail_on_stale_tools): the stdlib package half. The tree's generated
#projects name the package set, so a project without its package is
#refused by name (every import of that domain would otherwise fail at
#load time and read as a test-content bug), and each package is judged
#on its own stamp against ncc and the repo's newest stdlib source (the
#2026-10-31 lesson: a full sweep ran green against a stale package).
#The function only stats the tools, never runs them, so empty
#placeholder files laid out like a build root are enough; stamps are
#pinned with os.utime, never by sleeping.
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_e2e_tests as runner

#Pinned stamps on one timeline: a package at PACKAGE_STAMP is older than
#a source at NEW_SOURCE_STAMP and newer than one at OLD_SOURCE_STAMP.
EPOCH = 1
PACKAGE_STAMP = 1000
NEW_SOURCE_STAMP = 2000
OLD_SOURCE_STAMP = 500


def pin(path, stamp):
    os.utime(str(path), (stamp, stamp))


def make_tree(root, packages, missing=()):
    """Placeholder tools in the build-root layout the guard derives the
    stdlib dir from, a generated project per named domain, and a package
    per domain except those in `missing`, written last (fresh). Returns
    {tool: path}."""
    tools = {}
    for tool in ('ncc', 'nvm', 'ndb'):
        path = root / 'src' / 'tools' / tool / 'Release' / f'{tool}.cmd'
        path.parent.mkdir(parents=True)
        path.write_text('stub', encoding='utf-8')
        tools[tool] = str(path)
    (root / 'stdlib').mkdir(exist_ok=True)
    for domain in packages:
        (root / 'stdlib' / f'{domain}.nproj').write_text(
            '<Project/>', encoding='utf-8')
    for domain in packages:
        if domain not in missing:
            (root / 'stdlib' / f'{domain}.npkg').write_text(
                'stub package', encoding='utf-8')
    return tools


def refuse(tools):
    with pytest.raises(SystemExit) as exit_info:
        runner._fail_on_stale_tools(tools['ncc'], tools['nvm'], tools['ndb'])
    assert exit_info.value.code == 2


def test_fresh_domain_packages_pass(tmp_path):
    tools = make_tree(tmp_path, ('io', 'web'))
    runner._fail_on_stale_tools(tools['ncc'], tools['nvm'], tools['ndb'])


def test_project_without_its_package_is_refused(tmp_path, capsys):
    tools = make_tree(tmp_path, ('io', 'web'), missing=('web',))
    refuse(tools)
    out = capsys.readouterr().out
    assert 'web.npkg' in out
    assert 'nlang_stdlib_package' in out
    assert 'io.npkg' not in out


def test_stale_package_is_refused_with_its_bound(tmp_path, capsys):
    #Each package is judged on its own stamp: the fresh sibling is not
    #named, the stale one is, with the input it predates.
    tools = make_tree(tmp_path, ('io', 'web'))
    pin(tmp_path / 'stdlib' / 'web.npkg', EPOCH)
    refuse(tools)
    out = capsys.readouterr().out
    assert 'web.npkg' in out
    assert 'ncc.cmd' in out
    assert 'io.npkg' not in out


def test_tree_without_projects_is_not_judged(tmp_path):
    #An installed tool tree ships its packages beside the binaries, with
    #no generated project to name them.
    tools = make_tree(tmp_path, ())
    package = tmp_path / 'stdlib' / 'io.npkg'
    package.write_text('stub package', encoding='utf-8')
    pin(package, EPOCH)
    runner._fail_on_stale_tools(tools['ncc'], tools['nvm'], tools['ndb'])


def make_repo_with_submodule(root):
    """A repo whose newest stdlib source is a dotted sub-module under
    stdlib/web/ -- the web package's sub-module arm."""
    stdlib = root / 'stdlib'
    (stdlib / 'web').mkdir(parents=True)
    for rel in ('util.n', 'web/static.n'):
        (stdlib / rel).write_text('// stub source', encoding='utf-8')
    pin(stdlib / 'util.n', OLD_SOURCE_STAMP)
    return stdlib / 'web' / 'static.n'


def test_submodule_source_older_than_package_passes(
        tmp_path, monkeypatch):
    tools = make_tree(tmp_path, ('web',))
    static_n = make_repo_with_submodule(tmp_path / 'repo')
    pin(static_n, OLD_SOURCE_STAMP)
    #ncc predates the package, so the stdlib sources are the only bound
    #in play.
    pin(tools['ncc'], EPOCH)
    monkeypatch.setattr(runner, 'REPO_ROOT', str(tmp_path / 'repo'))
    pin(tmp_path / 'stdlib' / 'web.npkg', PACKAGE_STAMP)
    runner._fail_on_stale_tools(tools['ncc'], tools['nvm'], tools['ndb'])


def test_submodule_source_edit_stales_the_package(tmp_path, capsys,
                                                  monkeypatch):
    tools = make_tree(tmp_path, ('web',))
    static_n = make_repo_with_submodule(tmp_path / 'repo')
    pin(static_n, NEW_SOURCE_STAMP)
    pin(tools['ncc'], EPOCH)
    monkeypatch.setattr(runner, 'REPO_ROOT', str(tmp_path / 'repo'))
    pin(tmp_path / 'stdlib' / 'web.npkg', PACKAGE_STAMP)
    refuse(tools)
    out = capsys.readouterr().out
    assert 'web.npkg' in out
    assert 'static.n' in out
