# check_source_size_test.py -- self-test corpus for the guard: the
# signature detection and the allowlist config validation. Pins the
# separator rule the span heuristic depends on: a pointer/reference
# return may bind its * or & to either side ("Ret *Name(" is the
# repo's prevailing definition style and MUST be measured; a miss here
# silently exempts a whole function family from the <=50-line rule).
# Also pins the config self-defense: duplicate JSON keys and entries
# without path/reason must be refused (a silent duplicate-key merge
# once deleted a whole file's exemption — the guard that went red was
# the symptom, the config was the disease).
# Run: python check_source_size_test.py
# Verdict line: "GATE source_size_guard_selftest: PASS" (or FAIL),
# exit code follows. Per the ctest-override lesson the sentinel is
# computed from the assertions themselves, never printed unconditionally.

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_source_size import (function_spans, SIG_RE, _no_duplicate_keys,
                               validate_allowlist)


def make_unit(signature, body_lines):
    """A synthetic column-0 definition: signature, brace within 4 lines,
    body, closing brace."""
    return ([signature, "{"] + body_lines + ["}"])


def spans_of(lines):
    return list(function_spans(lines))


failures = []

def expect(label, lines, want_name, want_start, want_length):
    got = [(n, s, l) for n, s, l in spans_of(lines) if n == want_name]
    if len(got) != 1:
        failures.append("%s: expected exactly one span for %r, got %r"
                        % (label, want_name, got))
        return
    _, start, length = got[0]
    if start != want_start or length != want_length:
        failures.append("%s: expected (start %d, length %d), got (%d, %d)"
                        % (label, want_start, want_length, start, length))


def expect_absent(label, lines, want_name):
    got = [n for n, _, _ in spans_of(lines) if n == want_name]
    if got:
        failures.append("%s: %r must not be detected, got spans %r"
                        % (label, want_name, got))


# 1. THE HOLE: pointer return glued to the qualified name — the repo's
# prevailing definition style. Must be detected with the exact span.
expect("glued-pointer-qualified",
       make_unit("SnField *ExprResolveAccessor::ResolveIt(",
                 ["    return nullptr;"]),
       "ExprResolveAccessor::ResolveIt", 1, 4)

# 2. Plain value return keeps working.
expect("plain-qualified",
       make_unit("void Builder::Emit(const SnField &f)",
                 ["    (void)f;"]),
       "Builder::Emit", 1, 4)

# 3. Star bound to the return type instead (historical style).
expect("star-on-return-type",
       make_unit("static const char* Pick(const std::string &name)",
                 ["    return name.c_str();"]),
       "Pick", 1, 4)

# 4. Reference return glued to the name.
expect("reference-glued",
       make_unit("SnField &Pool::At(size_t i)",
                 ["    return m_Items[i];"]),
       "Pool::At", 1, 4)

# 5. Rvalue-reference return.
expect("rvalue-ref",
       make_unit("std::string &&Sink::Take()",
                 ["    return std::move(m_S);"]),
       "Sink::Take", 1, 4)

# 6. Spaces on both sides of the star.
expect("star-both-sides",
       make_unit("SnField * Resolver::Find(const std::string &n)",
                 ["    return nullptr;"]),
       "Resolver::Find", 1, 4)

# 7. Declarations (semicolon head) stay invisible.
expect_absent("declaration-skipped",
              ["SnField *Pool::At(size_t i);"],
              "Pool::At")

# 8. Keyword heads never phantom-match even with the wider separator.
expect_absent("namespace-head",
              ["namespace nlang", "{", "}"],
              "nlang")
expect_absent("if-head",
              ["if (ready)", "{", "    Run();", "}"],
              "ready")

# 9. An ALL-CAPS X-macro invocation stays invisible.
expect_absent("x-macro-invocation",
              ["OPCODE_LIST(X)"],
              "OPCODE_LIST")

# 10. Overloads of one name are measured separately (span ordering).
overloads = (
    ["SnField *Pool::Get(int i)", "{", "    return nullptr;", "}",
     "SnField *Pool::Get(const std::string &n)", "{",
     "    return nullptr;", "}"])
got = [(n, s, l) for n, s, l in spans_of(overloads) if n == "Pool::Get"]
if len(got) != 2 or [s for _, s, _ in got] != [1, 5]:
    failures.append("overloads: expected two spans starting 1 and 5, got %r"
                    % (got,))

# 11. Allowlist config self-defense (the silent-merge class: duplicate
# JSON keys are overwritten WITHOUT an error, and a pathless/reasonless
# entry exempts nothing while looking configured).
def expect_refused(label, fn):
    try:
        fn()
    except AssertionError:
        return
    failures.append("%s: AssertionError expected, call returned" % label)

expect_refused("duplicate-key-refused",
               lambda: json.loads('{"path": "a", "path": "b"}',
                                  object_pairs_hook=_no_duplicate_keys))
expect_refused("blank-path-refused",
               lambda: validate_allowlist([{"path": " ", "reason": "r"}]))
expect_refused("blank-reason-refused",
               lambda: validate_allowlist([{"path": "x.cpp", "reason": ""}]))

groups = validate_allowlist(
    [{"path": "a.cpp", "reason": "r1"},
     {"path": "a.cpp", "reason": "r2", "function": "F"},
     {"path": "b.cpp", "reason": "r3"}])
if ([e["reason"] for e in groups.get("a.cpp", [])] != ["r1", "r2"]
        or list(groups.get("b.cpp", []))[0]["reason"] != "r3"):
    failures.append("allowlist-grouping: expected per-file groups, got %r"
                    % (groups,))

if failures:
    print("source_size_guard selftest: %d failure(s)" % len(failures))
    for f in failures:
        print("  " + f)
    print("GATE source_size_guard_selftest: FAIL")
    sys.exit(1)
print("GATE source_size_guard_selftest: PASS")
