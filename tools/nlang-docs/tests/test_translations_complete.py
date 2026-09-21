"""Both nide .ts catalogs must be fully translated: a message left
"unfinished" leaks the authored string into that UI language (nide
ships both catalogs, there is no half-installed state). The catalogs
are hand-maintained XML (see src/tools/nide/CMakeLists.txt) and must
mirror each other's message sets."""
import re
import xml.etree.ElementTree as ET
from pathlib import Path

TS_DIR = (Path(__file__).resolve().parents[3] / "src" / "tools"
          / "nide" / "translations")
NIDE_DIR = TS_DIR.parent
CATALOGS = ["nide_zh.ts", "nide_en.ts"]
#Each catalog holds 170+ authored strings; a structural regression
#(empty parse, renamed file) must not pass as "zero unfinished".
MIN_MESSAGES = 150


def unfinished_messages(name, root):
    messages = root.findall(".//message")
    assert len(messages) >= MIN_MESSAGES, name
    #In the .ts format "unfinished" is an attribute of the
    #<translation> child; a message with no translation child counts
    #as untranslated too (lrelease reports it as such).
    unfinished = []
    for m in messages:
        tr = m.find("translation")
        if tr is not None and tr.get("type") != "unfinished":
            continue
        loc = m.find("location")
        unfinished.append((m.findtext("source"),
                           loc.get("line") if loc is not None else "?"))
    return unfinished


def test_no_unfinished_translations():
    for name in CATALOGS:
        root = ET.parse(TS_DIR / name).getroot()
        unfinished = unfinished_messages(name, root)
        assert not unfinished, (
            f"{name}: {len(unfinished)} unfinished, "
            f"first: {unfinished[:5]}")


def test_catalog_source_set_parity():
    #A (context, source) pair present in only one catalog either
    #comes back as unfinished at the next lupdate regeneration (see
    #CMakeLists.txt) or is silently untranslated by fallback.
    sets = {}
    for name in CATALOGS:
        root = ET.parse(TS_DIR / name).getroot()
        sets[name] = {
            (ctx.findtext("name"), m.findtext("source"))
            for ctx in root.findall("context")
            for m in ctx.findall("message")
        }
    zh_only = sets["nide_zh.ts"] - sets["nide_en.ts"]
    en_only = sets["nide_en.ts"] - sets["nide_zh.ts"]
    assert not zh_only and not en_only, (
        f"zh-only: {sorted(zh_only)[:5]}; en-only: {sorted(en_only)[:5]}")


def catalog_sources(name):
    root = ET.parse(TS_DIR / name).getroot()
    return {m.findtext("source")
            for ctx in root.findall("context")
            for m in ctx.findall("message")}


def authored_ui_strings():
    """Every translatable string the nide sources actually contain:
    tr("...") calls in .cpp/.h plus <string> properties in .ui files.
    One-way check — concatenated or multi-line tr() literals are not
    extracted (a permissive miss, never a false alarm). .ui entities
    are unescaped to match the XML-parsed catalog sources."""
    tr_re = re.compile(r'\btr\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
    found = {}
    for pattern in ("**/*.cpp", "**/*.h"):
        for path in NIDE_DIR.glob(pattern):
            for m in tr_re.finditer(path.read_text(encoding="utf-8")):
                found.setdefault(m.group(1), path)
    for path in sorted((NIDE_DIR / "ui").glob("*.ui")):
        for m in re.finditer(r"<string>([^<]+)</string>",
                             path.read_text(encoding="utf-8")):
            unescaped = (m.group(1)
                         .replace("&amp;", "&")
                         .replace("&lt;", "<")
                         .replace("&gt;", ">")
                         .replace("&quot;", '"')
                         .replace("&apos;", "'"))
            found.setdefault(unescaped, path)
    return found


def test_every_authored_string_is_in_both_catalogs():
    #Parity + unfinished checks cannot see a string that is missing
    #from BOTH catalogs (it then falls back untranslated everywhere) —
    #this caught "Toolbar icon size:" shipping English in the zh UI.
    authored = authored_ui_strings()
    assert len(authored) >= 150, "extraction collapsed; guard is blind"
    for name in CATALOGS:
        sources = catalog_sources(name)
        missing = {s: str(p) for s, p in authored.items()
                   if s not in sources}
        assert not missing, (
            f"{name}: {len(missing)} authored strings absent, "
            f"first: {sorted(missing.items())[:5]}")
