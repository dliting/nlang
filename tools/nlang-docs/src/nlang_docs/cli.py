"""CLI: build / serve / check.

build = mkdocs build --strict + site audit chained (a broken link
fails the build). serve = plain mkdocs serve for iteration. check =
audit only, for already-generated sites (verify_package reuses it).
"""
import argparse
import subprocess
import sys
from pathlib import Path

from . import __version__
from .linkcheck import check_site


def _mkdocs(args):
    #Shell out so this package never imports mkdocs itself.
    return subprocess.run(
        [sys.executable, "-m", "mkdocs", *args]).returncode


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="nlang-docs",
        description="NLang help-manual pipeline "
                    "(mkdocs wrapper + link audits)")
    parser.add_argument("--version", action="version",
                        version="nlang-docs " + __version__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_build = sub.add_parser(
        "build", help="mkdocs build --strict, then audit the site")
    p_build.add_argument("--config", required=True,
                         help="mkdocs.yml (absolute path)")
    p_build.add_argument("--site-dir", required=True,
                         help="output directory")

    p_serve = sub.add_parser(
        "serve", help="mkdocs serve (dev preview loop)")
    p_serve.add_argument("--config", required=True)

    p_check = sub.add_parser(
        "check", help="audit an already-generated site")
    p_check.add_argument("--site-dir", required=True)

    opts = parser.parse_args(argv)
    if opts.command == "build":
        rc = _mkdocs(["build", "--strict", "-f", opts.config,
                      "-d", opts.site_dir])
        if rc != 0:
            return rc
        return check_site(Path(opts.site_dir))
    if opts.command == "serve":
        return _mkdocs(["serve", "-f", opts.config])
    if opts.command == "check":
        return check_site(Path(opts.site_dir))
    #argparse required=True makes this unreachable today; returning
    #explicitly beats silently falling into another subcommand later.
    return None
