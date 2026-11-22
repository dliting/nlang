# gate_time_carrier.py -- the carrier axis (docs/dev/gate_timing_design.md
# D-6, guard eleven as narrowed by D-16/D-17): check-carrier pairs every
# row of a slow member and every ASan/Release row with a leg of the batch
# itself that runs the entry under the row's own config key (regular
# Debug/IDE rows are carried by construction: the regular legs run
# whatever the tree enumerates except the slow members). It reads the
# name<TAB>command lines gate_tier.sh records in
# leg_commands.tsv and reconstructs each leg's judging config from the
# state its own command attests to: a ctest leg's key set is the tree's
# CMakeCache.txt shape converted by -C (the same predicate
# tests/gate_time_keys.cmake derives from -- F-1: -C Release cannot
# promote a sanitizer tree out of ctest-ASan), an e2e leg's is the
# explicit --config= VALUE (a tool-path prefix pairs no config). Run
# sets come from the tree's ctest --show-only enumeration filtered by
# the leg's own -R/-E, from the perf differential file ∩ -R for the
# perf legs (whose ON-cache window has closed before this checker
# runs), and from the manifest files an e2e leg reads plus those
# files' BASENAMES (a scope row pairs against the file name, never an
# entry line). A row without its config's carrier, a mispaired leg
# config and a missing input are all ordinary findings naming what is
# wrong. Reached through gate_time_schedule's hooks (R-28: the entry
# maps names, schedule owns the registration, this module owns the
# check). Stdlib only; run with -I.

import re
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_table import load_table
from gate_time_timeouts import read_timeouts, show_only_json

# The --config= values an e2e leg may spell out (the e2e config keys
# minus their prefix -- the runner's own build-config vocabulary).
E2E_CONFIG_VALUES = ("Debug", "ASan", "Release")


def cache_values(path):
    """A CMakeCache.txt parsed to {name: (type, value)}: the config-shape
    predicate reads CMAKE_CXX_FLAGS (the sanitizer flag) and
    NLANG_BUILD_IDE, and print-perf-names forwards the non-INTERNAL
    entries to its scratch configures. Malformed lines are skipped --
    they carry no cache entry, and a missing entry reads as missing
    rather than wrong."""
    values = {}
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return values
    for line in text.splitlines():
        name, sep, rest = line.partition(":")
        if not sep:
            continue
        type_name, sep, value = rest.partition("=")
        if not sep:
            continue
        values[name] = (type_name, value)
    return values


def _native_form(text):
    """A path as bash left it in the recorded command. gate_tier.sh's
    $REPO comes back from `pwd` in MSYS mount form (/e/cases/...), so
    every tree, manifest and scratch path IT derived rode the leg
    commands that way; drive mounts map back to drive letters here so
    the checker can open the file. A caller whose paths are native
    Windows strings passes through untouched."""
    hit = re.match(r"^/([a-zA-Z])/", text)
    return "%s:/%s" % (hit.group(1), text[2:]) if hit else text


def flatten(command):
    """The leg command as one flat token list. The first shlex pass
    consumes the leg invocation's own quoting; a second pass runs on
    any token that still carries a command chain -- the `bash -c`
    script arrives either wrapped in double quotes (whole token, the
    shape the corpus pins) or already rejoined by the recorder with
    only its single-quoted paths left inside (the shape the tier
    echoes); the `&&` marker says which tokens those are either way."""
    flat = []
    for token in shlex.split(command):
        if " && " in token:
            flat.extend(shlex.split(token))
        else:
            flat.append(token)
    return flat


def _value_after(tokens, flag):
    for index, token in enumerate(tokens):
        if token == flag and index + 1 < len(tokens):
            return tokens[index + 1]
    return None


def _tree_and_config(name, tokens, findings):
    """(tree, config key) a ctest leg judges under, or (None, None) after
    a named finding: tree from `cd <dir>`/`-B <dir>`, config key from
    the cache shape (fsanitize -> ctest-ASan, NLANG_BUILD_IDE ->
    ctest-IDE, else ctest-Debug), `-C Release` converting only a
    non-sanitizer tree to ctest-Release (the key set follows the shape,
    F-1). The same shape predicate tests/gate_time_keys.cmake applies
    at configure time; this is its read side."""
    tree = _value_after(tokens, "-B") or _value_after(tokens, "cd")
    if tree is None:
        findings.append("CARRIER %s: the leg command names no build tree "
                        "(neither `cd <dir>` nor `-B <dir>` carries its "
                        "config shape)" % name)
        return None, None
    tree = Path(_native_form(tree))
    cache = tree / "CMakeCache.txt"
    if not cache.is_file():
        findings.append("CARRIER %s: %s is missing -- a ctest leg's "
                        "judging config key comes from the tree's cache "
                        "shape" % (name, cache))
        return None, None
    values = cache_values(cache)
    asan = "fsanitize=address" in values.get("CMAKE_CXX_FLAGS",
                                             ("", ""))[1]
    ide = values.get("NLANG_BUILD_IDE", ("", ""))[1].upper() in (
        "ON", "1", "Y", "YES", "TRUE")
    config_flag = _value_after(tokens, "-C")
    if config_flag == "Release":
        if asan:
            findings.append(
                "CARRIER %s: -C Release cannot pair a ctest-Release row "
                "against a sanitizer-shaped tree -- the shape keeps the "
                "leg's config key at ctest-ASan (F-1: the key set is "
                "derived from the cache, not from -C)" % name)
            return None, None
        return tree, "ctest-Release"
    if config_flag == "Debug":
        if asan:
            return tree, "ctest-ASan"
        return tree, ("ctest-IDE" if ide else "ctest-Debug")
    findings.append("CARRIER %s: a ctest leg must spell out -C Debug or "
                    "-C Release -- rows pair against the config the leg "
                    "judges under, never a guess" % name)
    return None, None


def _perf_leg_names(name, tokens, perf_names, findings):
    """The perf leg's run set: the print-perf-names differential ∩ the
    leg's -R. Enumeration is no option -- by the time this checker runs
    the cache is restored to OFF and the tree holds none of these
    entries, so a checker that enumerated would read an innocent empty
    set and green-light a missing carrier leg."""
    if perf_names is None:
        findings.append(
            "CARRIER %s: an NLANG_PERF_TESTS=ON leg needs --perf-names "
            "(the registration differential) to know its run set -- the "
            "restored cache enumerates its entries empty" % name)
        return set()
    regex = _value_after(tokens, "-R")
    if regex is None:
        findings.append("CARRIER %s: the perf leg selects its entries "
                        "with -R (its run set is the differential ∩ -R)"
                        % name)
        return set()
    return {n for n in perf_names if re.search(regex, n)}


def _enumerated_names(name, tokens, tree, findings):
    """A regular ctest leg's run set: the tree's ctest --show-only
    enumeration under ctest's own filter semantics (entries -R hits,
    minus entries -E hits -- F-12), never a re-derivation of it. A
    failed enumeration is the leg's finding, not a checker crash."""
    try:
        names = set(read_timeouts(show_only_json(tree)))
    except ValueError as exc:
        findings.append("CARRIER %s: %s" % (name, exc))
        return set()
    regex = _value_after(tokens, "-R")
    if regex is not None:
        names = {n for n in names if re.search(regex, n)}
    exclude = _value_after(tokens, "-E")
    if exclude is not None:
        names = {n for n in names if not re.search(exclude, n)}
    return names


def _e2e_leg(name, tokens, findings):
    """(config key, run set) for an e2e leg. The config key is the
    explicit --config= VALUE: the ASan sweep runs instrumented-Debug
    binaries while its rows sit under e2e-ASan, so a tool-path prefix
    pairs no config and an absent flag has no key to pair against. The
    run set is what the runner reads -- the NLANG_E2E_MANIFEST file,
    else the script directory's manifest.txt plus examples_manifest.txt
    (the examples file's absence is tolerated exactly as the runner
    tolerates it) -- entry names plus each manifest file's BASENAME,
    which is what guard eleven's scope rows pair against."""
    config = None
    for token in tokens:
        if not token.startswith("--config="):
            continue
        value = token.split("=", 1)[1]
        if value in E2E_CONFIG_VALUES:
            config = "e2e-" + value
        else:
            findings.append(
                "CARRIER %s: --config=%s is not a judging config value "
                "(want %s)" % (name, value, ", ".join(E2E_CONFIG_VALUES)))
        break
    else:
        findings.append(
            "CARRIER %s: the e2e leg spells out no --config=<%s> value "
            "-- rows pair by VALUE, never by a tool-path prefix"
            % (name, "|".join(E2E_CONFIG_VALUES)))
    script = next((t for t in tokens
                   if t.endswith("run_e2e_tests.py")), None)
    env = next((t for t in tokens
                if t.startswith("NLANG_E2E_MANIFEST=")), None)
    if env is not None:
        paths = [Path(_native_form(env.split("=", 1)[1]))]
    else:
        script_dir = Path(_native_form(script)).parent if script else None
        paths = ([script_dir / "manifest.txt",
                  script_dir / "examples_manifest.txt"] if script_dir
                 else [])
    names = set()
    for path in paths:
        if not path.is_file():
            if path.name == "examples_manifest.txt":
                continue
            findings.append("CARRIER %s: manifest %s does not exist -- "
                            "the e2e run set is exactly what the runner "
                            "reads" % (name, path))
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                names.add(line.split()[0])
        names.add(path.name)
    return config, names


def _observed(args, leg_lines, findings):
    """{config key: entries} over the batch's legs: e2e legs from their
    manifest, NLANG_PERF_TESTS=ON legs from the differential file,
    regular ctest legs from the tree enumeration; every other leg
    (checkers, cmake --build, the OFF-flag restore) pairs nothing and
    is skipped."""
    perf_names = None
    if args.perf_names:
        path = Path(args.perf_names)
        if not path.is_file():
            findings.append("CARRIER: --perf-names file %s does not "
                            "exist" % path)
        else:
            perf_names = {n for n in re.split(
                r"[|\r\n]+", path.read_text(encoding="utf-8")) if n}
    observed = {}
    for name, command in leg_lines:
        tokens = flatten(command)
        is_e2e = any(t.endswith("run_e2e_tests.py") for t in tokens)
        is_on = any(t.endswith("NLANG_PERF_TESTS=ON") for t in tokens)
        is_ctest = any(Path(t).name in ("ctest", "ctest.exe")
                       for t in tokens)
        if is_e2e:
            config, names = _e2e_leg(name, tokens, findings)
        elif is_on or is_ctest:
            tree, config = _tree_and_config(name, tokens, findings)
            if config is None:
                continue
            if is_on:
                names = _perf_leg_names(name, tokens, perf_names, findings)
            else:
                names = _enumerated_names(name, tokens, tree, findings)
        else:
            continue
        if config is not None:
            observed.setdefault(config, set()).update(names)
    return observed


def run(args, table, verdict):
    """Guard eleven's verdict (D-17): every row of a slow member (the
    one set gate_time_schedule.slow_names derives from the baseline-row
    seconds) and every ASan/Release row must sit in its own config key's
    observed run set -- the same entry name under another config proves
    nothing, and every mispaired or under-determined leg already
    contributed its own finding. The `default` coarse rows price budgets
    rather than run, so they pair against nothing."""
    findings = []
    path = Path(args.leg_commands)
    if not path.is_file():
        return verdict(["CARRIER: %s does not exist -- the batch records "
                        "one `name<TAB>command` line per leg and the "
                        "pairing has no input" % path])
    leg_lines = []
    for line in path.read_text(encoding="utf-8").splitlines():
        name, sep, command = line.partition("\t")
        if sep:
            leg_lines.append((name, command))
    observed = _observed(args, leg_lines, findings)
    # Membership is the schedule axis' single judgment (D-17), imported
    # here rather than at module head: gate_time_schedule dispatches
    # into this module, so a head import would be circular.
    from gate_time_schedule import slow_names
    rows = load_table(Path(table))
    slow = set(slow_names(rows))
    for row in rows:
        if row.entry == "default":
            continue
        # Regular Debug/IDE rows are carried by construction (the
        # regular legs run the tree's enumeration minus the slow
        # members), so pairing them would prove nothing; the rows that
        # need a named carrier are the slow members in every config and
        # the sanitizer/Release rows, whose legs run in other batches
        # and can silently drop entries (D-17).
        if not (row.entry in slow or row.config.endswith("-ASan")
                or row.config.endswith("-Release")):
            continue
        if row.entry not in observed.get(row.config, ()):
            findings.append(
                "CARRIER %s %s: no leg of this batch runs it under this "
                "config key -- a slow member or sanitizer/Release row "
                "without its carrier is an entry that silently stops "
                "being tested" % (row.config, row.entry))
    return verdict(findings)


def add_subparsers(sub, table_arg):
    """check-carrier's parser; the schedule module hands it its --table
    parent so the flag definition stays shared across subcommands
    (R-28)."""
    carrier_p = sub.add_parser(
        "check-carrier", parents=[table_arg],
        help="guard eleven: pair every slow-member row and every "
             "ASan/Release row with a leg that ran it under the row's "
             "own config key")
    carrier_p.add_argument("--leg-commands", required=True, metavar="TSV",
                           help="the batch's leg_commands.tsv "
                                "(name<TAB>command lines)")
    carrier_p.add_argument("--perf-names", metavar="FILE",
                           help="the print-perf-names differential file "
                                "(the perf legs' run set once the cache "
                                "is restored to OFF)")
