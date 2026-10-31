#!/usr/bin/env python3
"""Validate calligrus's shared test data (spec/tests/) and built-in themes (themes/).

This file is also the *reference implementation* of the rules in README.md: it
resolves every config-*.toml, runs every edit-*.ops script through an in-place
writer, resolves the theme-*.toml files and every line of syntax-lookup.txt, and
compares the results with the expected files. It also checks that every built-in
theme has the full key set of calligrus-dark.toml and prints a contrast report.
It also answers every query of the fontpick-*.txt files (font picking by family
name, SPEC.md §8a; --regen rewrites their expected answers).

    python check.py            # check everything, exit code 1 on any failure
    python check.py --regen    # rewrite the expected files from this implementation
                               # (review the diff by hand before keeping it!)
    python check.py --quiet    # no contrast table

Needs Python 3.11+ (tomllib).
"""
import math
import re
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent  # the repository root
THEMES_DIR = ROOT / "themes"
THEME_TXT = ROOT / "grammars" / "theme.txt"

FAILURES = []


def fail(msg):
    FAILURES.append(msg)
    print("FAIL:", msg)


# =============================================================================
# TOML value formatting (used by the .expected files and by the writer)
# =============================================================================

def fmt_string(s):
    """TOML basic string: escape backslash, double quote and control characters."""
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch in "\b\t\n\f\r":
            out.append({"\b": "\\b", "\t": "\\t", "\n": "\\n", "\f": "\\f", "\r": "\\r"}[ch])
        elif o < 0x20 or o == 0x7F:
            out.append(f"\\u{o:04X}")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def fmt_float(x):
    """Shortest text that reads back as the same double, always with a '.'."""
    r = repr(float(x))
    if "e" in r or "E" in r or "n" in r:  # not needed for calligrus's ranges
        raise ValueError(f"unsupported float {x!r}")
    return r if "." in r else r + ".0"


def fmt_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return fmt_float(v)
    if isinstance(v, str):
        return fmt_string(v)
    raise ValueError(f"unsupported value type {type(v).__name__}")


# =============================================================================
# Config resolution
# =============================================================================

def _theme_name(v):
    return isinstance(v, str) and v != ""


def _font_path(v):
    return isinstance(v, str)


def _bool(v):
    return isinstance(v, bool)


def _zoom(v):
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return False
    return math.isfinite(v) and 0.5 <= v <= 3.0


def _body_size(v):
    return isinstance(v, int) and not isinstance(v, bool) and 12 <= v <= 28


def _max_width(v):
    return isinstance(v, int) and not isinstance(v, bool) and 400 <= v <= 2400


# (table or None, key, default, validator, converter)
CONFIG_KEYS = [
    (None, "theme", "auto", _theme_name, str),
    (None, "dark_theme", "calligrus-dark", _theme_name, str),
    (None, "light_theme", "calligrus-light", _theme_name, str),
    (None, "zoom", 1.0, _zoom, float),
    (None, "body_size", 16, _body_size, int),
    (None, "max_width", 860, _max_width, int),
    (None, "show_outline", True, _bool, bool),
    (None, "remember_window", True, _bool, bool),
    (None, "editor_marker", True, _bool, bool),
    (None, "vim_keys", True, _bool, bool),
    ("fonts", "prose", "", _font_path, str),
    ("fonts", "prose_bold", "", _font_path, str),
    ("fonts", "prose_italic", "", _font_path, str),
    ("fonts", "prose_bold_italic", "", _font_path, str),
    ("fonts", "mono", "", _font_path, str),
    ("code", "line_numbers", False, _bool, bool),
]


def resolve_config(data: bytes):
    """Return {dotted key: value} with every setting resolved."""
    try:
        doc = tomllib.loads(data.decode("utf-8"))
    except (tomllib.TOMLDecodeError, UnicodeDecodeError):
        doc = {}  # a broken file never prevents start-up: all defaults
    out = {}
    for table, key, default, ok, conv in CONFIG_KEYS:
        src = doc if table is None else doc.get(table)
        dotted = key if table is None else f"{table}.{key}"
        if isinstance(src, dict) and key in src and ok(src[key]):
            out[dotted] = conv(src[key])
        else:
            out[dotted] = default
    return out


def config_expected_text(resolved):
    return "".join(f"{k} = {fmt_value(resolved[k])}\n" for k in sorted(resolved))


# =============================================================================
# In-place config writer
# =============================================================================

def split_lines(text):
    """[(content, eol)] with eol in {"\\n", "\\r\\n", ""}; no phantom last line."""
    lines = []
    i = 0
    while i < len(text):
        j = text.find("\n", i)
        if j < 0:
            lines.append((text[i:], ""))
            break
        if j > i and text[j - 1] == "\r":
            lines.append((text[i:j - 1], "\r\n"))
        else:
            lines.append((text[i:j], "\n"))
        i = j + 1
    return lines


def header_name(content):
    s = content.lstrip(" \t")
    if not s.startswith("["):
        return None
    end = s.find("]")
    if end < 0:
        return None
    return s[1:end].strip(" \t")


def is_blank(content):
    return content.strip(" \t") == ""


def match_key(content, key):
    """Index just after '=' if this line defines `key` (bare or "quoted"), else None."""
    s = content.lstrip(" \t")
    indent = len(content) - len(s)
    for form in (key, f'"{key}"'):
        if s.startswith(form):
            rest = s[len(form):]
            r = rest.lstrip(" \t")
            if r.startswith("="):
                return indent + len(form) + (len(rest) - len(r)) + 1
    return None


def value_end(content, start):
    """End index of the TOML value starting at `start` (single-line values only)."""
    if start >= len(content):
        return start
    c = content[start]
    if c == '"':
        i = start + 1
        while i < len(content):
            if content[i] == "\\":
                i += 2
                continue
            if content[i] == '"':
                return i + 1
            i += 1
        return len(content)
    if c == "'":
        j = content.find("'", start + 1)
        return len(content) if j < 0 else j + 1
    i = start
    while i < len(content) and content[i] not in " \t#":
        i += 1
    return i


def replace_value(content, eq, new):
    vstart = eq
    while vstart < len(content) and content[vstart] in " \t":
        vstart += 1
    vend = value_end(content, vstart)
    old = content[vstart:vend]
    rest = content[vend:]
    hash_at = rest.find("#")
    if hash_at >= 0 and is_blank(rest[:hash_at]):
        gap, comment = rest[:hash_at], rest[hash_at:]
        if gap != "" and set(gap) == {" "}:
            gap = " " * max(1, len(gap) + len(old) - len(new))
        rest = gap + comment
    return content[:vstart] + new + rest


def apply_set(text, dotted, value):
    bom = ""
    if text.startswith("﻿"):
        bom, text = "﻿", text[1:]
    table, key = (None, dotted) if "." not in dotted else dotted.split(".", 1)
    lines = split_lines(text)
    eol = next((e for _, e in lines if e), "\n")
    new_value = fmt_value(value)

    # Sections: (name, header index or -1, first line, end line exclusive)
    headers = [i for i, (c, _) in enumerate(lines) if header_name(c) is not None]
    sections = [(None, -1, 0, headers[0] if headers else len(lines))]
    for n, h in enumerate(headers):
        end = headers[n + 1] if n + 1 < len(headers) else len(lines)
        sections.append((header_name(lines[h][0]), h, h + 1, end))

    section = next((s for s in sections if s[0] == table), None)
    if section is not None:
        _, h, first, end = section
        for i in range(first, end):
            eq = match_key(lines[i][0], key)
            if eq is not None:
                lines[i] = (replace_value(lines[i][0], eq, new_value), lines[i][1])
                return bom + "".join(c + e for c, e in lines)
        # Not found: insert after the section's last non-blank line.
        last = h  # header line (or -1 for the top-level section = start of file)
        for i in range(first, end):
            if not is_blank(lines[i][0]):
                last = i
        if last >= 0 and lines[last][1] == "":
            lines[last] = (lines[last][0], eol)
        lines.insert(last + 1, (f"{key} = {new_value}", eol))
        return bom + "".join(c + e for c, e in lines)

    # The table does not exist: append header + key at the end of the file.
    if lines:
        if lines[-1][1] == "":
            lines[-1] = (lines[-1][0], eol)
        if not is_blank(lines[-1][0]):
            lines.append(("", eol))
    lines.append((f"[{table}]", eol))
    lines.append((f"{key} = {new_value}", eol))
    return bom + "".join(c + e for c, e in lines)


def parse_ops(text):
    ops = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = re.fullmatch(r"set ([A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)?) (.+)", line)
        if not m:
            raise ValueError(f"bad op on line {n}: {line!r}")
        value = tomllib.loads("v = " + m.group(2))["v"]
        ops.append((m.group(1), value))
    return ops


def run_edit(toml_bytes, ops_bytes):
    text = toml_bytes.decode("utf-8")
    for dotted, value in parse_ops(ops_bytes.decode("utf-8")):
        text = apply_set(text, dotted, value)
    return text.encode("utf-8")


# =============================================================================
# Themes
# =============================================================================

UI_KEYS = ["panel", "border", "widget", "widget_hover", "widget_active", "text", "weak",
           "field", "selection", "selection_text", "accent"]
DOC_KEYS = ["bg", "text", "weak", "link", "heading", "code_bg", "code_border", "code_fg",
            "rule", "quote_bar", "table_head_bg", "zebra", "error", "find_match",
            "find_current"]
COLOUR_RE = re.compile(r"#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})")


def norm_colour(v):
    """'#RRGGBB' / '#rrggbbaa' -> '#rrggbbaa' (lower case), else None."""
    if not isinstance(v, str) or not COLOUR_RE.fullmatch(v):
        return None
    v = v.lower()
    return v if len(v) == 9 else v + "ff"


def parse_style(v):
    """'#rrggbb [bold] [italic]' -> (colour, bold, italic), or None if the colour is bad."""
    if not isinstance(v, str):
        return None
    words = v.split()
    if not words:
        return None
    colour = norm_colour(words[0])
    if colour is None:
        return None
    return colour, "bold" in words[1:], "italic" in words[1:]


def load_toml(path):
    with open(path, "rb") as f:
        return tomllib.load(f)


BUILTIN = {p.stem: load_toml(p) for p in sorted(THEMES_DIR.glob("*.toml"))}


def resolve_theme(t):
    """All [ui] / [doc] colours as {"ui.panel": "#rrggbbaa", ...}."""
    base = BUILTIN["calligrus-dark" if t.get("dark") is True else "calligrus-light"]
    out = {}
    for table, keys in (("ui", UI_KEYS), ("doc", DOC_KEYS)):
        own = t.get(table) if isinstance(t.get(table), dict) else {}
        for k in keys:
            c = norm_colour(own.get(k))
            if c is None and table == "doc" and k == "heading":
                c = out["doc.text"]  # heading defaults to this theme's own text colour
            if c is None:
                c = norm_colour(base[table][k])
            out[f"{table}.{k}"] = c
    # heading_1 .. heading_6: optional, never from the base theme; missing or
    # invalid -> doc.heading (h6: doc.weak).
    own = t.get("doc") if isinstance(t.get("doc"), dict) else {}
    for i in range(1, 7):
        c = norm_colour(own.get(f"heading_{i}"))
        out[f"doc.heading_{i}"] = c or out["doc.weak" if i == 6 else "doc.heading"]
    # bold / italic / quote: optional too, never from the base theme; missing or
    # invalid -> doc.text (bold, italic) / doc.weak (quote).
    for k, fallback in (("bold", "text"), ("italic", "text"), ("quote", "weak")):
        out[f"doc.{k}"] = norm_colour(own.get(k)) or out[f"doc.{fallback}"]
    return out


def theme_expected_text(resolved):
    return "".join(f"{k} = {resolved[k]}\n" for k in sorted(resolved))


def load_theme_txt():
    entries = {}
    for line in THEME_TXT.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        entries[parts[0]] = parts[1] != "-"  # False = "plain" entry
    return entries


THEME_TXT_ENTRIES = load_theme_txt()


def dotted_prefixes(name):
    while True:
        yield name
        if "." not in name:
            return
        name = name[:name.rfind(".")]


def lookup_syntax(theme, capture):
    """Result string for syntax-lookup.txt."""
    if capture.startswith("_"):
        return "plain"
    entry = next((p for p in dotted_prefixes(capture) if p in THEME_TXT_ENTRIES), None)
    if entry is None or not THEME_TXT_ENTRIES[entry]:
        return "plain"
    table = theme.get("syntax") if isinstance(theme.get("syntax"), dict) else {}
    for p in dotted_prefixes(entry):
        style = parse_style(table.get(p))
        if style is not None:
            colour, bold, italic = style
            return colour + (" bold" if bold else "") + (" italic" if italic else "")
    return "plain"


# =============================================================================
# Contrast (WCAG 2.x)
# =============================================================================

def rgba(c):
    c = norm_colour(c)
    return tuple(int(c[i:i + 2], 16) for i in (1, 3, 5, 7))


def over(fg, bg):
    """Composite fg (may have alpha) over an opaque bg -> (r, g, b)."""
    r, g, b, a = rgba(fg)
    br, bgg, bb, _ = rgba(bg)
    a /= 255
    return tuple(round(a * x + (1 - a) * y) for x, y in ((r, br), (g, bgg), (b, bb)))


def luminance(rgb):
    def ch(v):
        v /= 255
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (ch(v) for v in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def hexrgb(rgb):
    return "#" + "".join(f"{v:02x}" for v in rgb) + "ff"


def theme_contrasts(t):
    ui, doc = t["ui"], t["doc"]
    bg = hexrgb(over(doc["bg"], "#000000"))
    panel = hexrgb(over(ui["panel"], "#000000"))
    on_bg = lambda c: over(c, bg)
    code_bg = hexrgb(on_bg(doc["code_bg"]))
    text = on_bg(doc["text"])
    comment = parse_style(t["syntax"]["comment"])[0]
    return {
        "text": contrast(text, on_bg(bg)),
        "weak": contrast(on_bg(doc["weak"]), on_bg(bg)),
        "link": contrast(on_bg(doc["link"]), on_bg(bg)),
        "heading": contrast(on_bg(doc["heading"]), on_bg(bg)),
        # the hardest-to-read per-level heading colour (heading_1 .. heading_6, when given)
        "h_levels": min(contrast(on_bg(doc[f"heading_{i}"]), on_bg(bg)) for i in range(1, 7) if f"heading_{i}" in doc)
                    if any(f"heading_{i}" in doc for i in range(1, 7)) else contrast(on_bg(doc["heading"]), on_bg(bg)),
        # the hardest-to-read of bold / italic / quote text, when given
        "emph": min([contrast(on_bg(doc[k]), on_bg(bg)) for k in ("bold", "italic", "quote") if k in doc], default=99.0),
        "code_fg": contrast(over(doc["code_fg"], code_bg), over(code_bg, bg)),
        "comment": contrast(over(comment, code_bg), over(code_bg, bg)),
        "on_match": contrast(text, on_bg(doc["find_match"])),
        "on_current": contrast(text, on_bg(doc["find_current"])),
        "ui_text": contrast(over(ui["text"], panel), over(panel, panel)),
        "ui_weak": contrast(over(ui["weak"], panel), over(panel, panel)),
        "sel_text": contrast(over(ui["selection_text"], panel), over(ui["selection"], panel)),
    }


# Minimums checked for every built-in theme (the three original themes predate
# this table, so they only get warnings).
MINIMUMS = {"text": 4.5, "weak": 3.0, "link": 3.0, "heading": 3.0, "h_levels": 3.0, "emph": 3.0, "code_fg": 3.0,
            "on_match": 4.5, "on_current": 3.0, "ui_text": 4.5, "sel_text": 4.5}
LEGACY = {"calligrus-dark", "calligrus-light", "leuven"}


# =============================================================================
# Font picking by family name (SPEC.md §8a, README.md §5)
# =============================================================================

FONT_SUFFIXES = (".ttf", ".otf", ".ttc")


def font_value_kind(value):
    """'name' for a family name, 'path' for a font file path (§8a)."""
    if "/" in value or "\\" in value or value.lower().endswith(FONT_SUFFIXES):
        return "path"
    return "name"


def ascii_lower(s):
    """Family names compare case-insensitively in ASCII only (README §5)."""
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


class Face:
    def __init__(self, family, subfamily, weight, italic, mono, file, index, ref, width=5):
        self.family, self.subfamily = family, subfamily
        self.weight, self.mono = weight, mono
        self.width = width if 1 <= width <= 9 else 5  # usWidthClass; out of range -> normal
        sub = ascii_lower(subfamily)
        # The italic column, or a subfamily containing "Italic"/"Oblique".
        self.italic = italic or "italic" in sub or "oblique" in sub
        self.file, self.index, self.ref = file, index, ref


def parse_face_line(line):
    """'family | subfamily | weight | italic | mono | file#face [| width]' -> Face, or raise ValueError."""
    parts = [p.strip(" \t") for p in line.split("|")]
    if len(parts) not in (6, 7):
        raise ValueError("expected 6 or 7 fields separated by '|'")
    width = 5
    if len(parts) == 7:
        if not re.fullmatch(r"[0-9]+", parts[6]):
            raise ValueError(f"bad width {parts[6]!r}")
        width = int(parts[6])
    family, subfamily, weight, italic, mono, ref = parts[:6]
    if not family:
        raise ValueError("empty family")
    if not re.fullmatch(r"[0-9]+", weight) or not 1 <= int(weight) <= 1000:
        raise ValueError(f"bad weight {weight!r}")
    if italic not in ("0", "1") or mono not in ("0", "1"):
        raise ValueError("italic and mono must be 0 or 1")
    m = re.fullmatch(r"(.+)#([0-9]+)", ref)
    if not m:
        raise ValueError(f"bad file#face {ref!r}")
    return Face(family, subfamily, int(weight), italic == "1", mono == "1",
                m.group(1), int(m.group(2)), ref, width)


def closest(faces, target, minimum=0):
    """Among the style's candidates (weight >= minimum): width closest to 5 first,
    then weight closest to `target`; ties: lower weight, then file path in byte
    order, then face index. Equal width distance (4 vs 6) is a tie on width."""
    cands = [f for f in faces if f.weight >= minimum]
    if not cands:
        return None
    return min(cands, key=lambda f: (abs(f.width - 5), abs(f.weight - target), f.weight,
                                     f.file.encode("utf-8"), f.index))


def family_faces(index, family):
    key = ascii_lower(family)
    return [f for f in index if ascii_lower(f.family) == key]


def pick_family(index, family):
    """{'regular': Face|None, 'bold': ..., 'italic': ..., 'bold_italic': ...} or None if unknown."""
    faces = family_faces(index, family)
    if not faces:
        return None
    upright = [f for f in faces if not f.italic]
    italic = [f for f in faces if f.italic]
    return {
        # A family with no upright face at all uses its italic closest to 400 as regular.
        "regular": closest(upright, 400) or closest(italic, 400),
        "bold": closest(upright, 700, 600),
        "italic": closest(italic, 400),
        "bold_italic": closest(italic, 700, 600),
    }


def list_families(index, mono_only):
    """Distinct families (case-insensitive), spelled as on their regular face, sorted."""
    groups = {}
    for f in index:
        groups.setdefault(ascii_lower(f.family), []).append(f)
    names = []
    for key, faces in groups.items():
        if mono_only and not any(f.mono for f in faces):
            continue
        names.append(pick_family(index, faces[0].family)["regular"].family)
    return sorted(names, key=lambda n: (ascii_lower(n), n.encode("utf-8")))


def fontpick_answer(index, kind, arg):
    if kind == "kind":
        return font_value_kind(arg)
    if kind == "pick":
        picks = pick_family(index, arg)
        if picks is None:
            return "not found"
        return " ".join(f"{s}={picks[s].ref if picks[s] else 'none'}"
                        for s in ("regular", "bold", "italic", "bold_italic"))
    if kind == "families":
        if arg not in ("mono", "all"):
            raise ValueError(f"families takes 'mono' or 'all', not {arg!r}")
        names = list_families(index, arg == "mono")
        return " | ".join(names) if names else "-"
    raise ValueError(f"unknown query {kind!r}")


def check_fontpick(path, regen):
    lines = path.read_text(encoding="utf-8").splitlines()
    index, queries = [], []
    for n, line in enumerate(lines):
        if not line.strip() or line.startswith("#"):
            continue
        if " => " in line:
            queries.append(n)
            continue
        try:
            index.append(parse_face_line(line))
        except ValueError as e:
            fail(f"{path.name}:{n + 1}: bad face line ({e}): {line!r}")
    out = list(lines)
    for n in queries:
        line = lines[n]
        head, want = line.rsplit(" => ", 1)
        m = re.fullmatch(r"(kind|pick|families) (.+)", head)
        if not m:
            fail(f"{path.name}:{n + 1}: malformed query {line!r}")
            continue
        try:
            got = fontpick_answer(index, m.group(1), m.group(2))
        except ValueError as e:
            fail(f"{path.name}:{n + 1}: {e}")
            continue
        out[n] = f"{head} => {got}"
        if not regen and got != want:
            fail(f"{path.name}:{n + 1}: {head} => {want}, reference gives {got}")
    if regen:
        path.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
        print("wrote", path.name)


# =============================================================================
# Checks
# =============================================================================

def check_file(path, actual: bytes, regen):
    if regen:
        path.write_bytes(actual)
        print("wrote", path.name)
        return
    if not path.exists():
        fail(f"{path.name} is missing")
        return
    expected = path.read_bytes()
    if expected != actual:
        fail(f"{path.name} differs from the reference result:\n--- expected\n"
             f"{expected!r}\n--- actual\n{actual!r}")


def check_themes(quiet):
    ref = BUILTIN["calligrus-dark"]
    ref_keys = {t: set(ref[t]) for t in ("ui", "doc", "syntax")}
    rows = []
    for name, t in BUILTIN.items():
        if t.get("name") != name:
            fail(f"themes/{name}.toml: name = {t.get('name')!r}, expected the file stem")
        if not isinstance(t.get("display_name"), str) or not t["display_name"]:
            fail(f"themes/{name}.toml: display_name missing")
        if not isinstance(t.get("dark"), bool):
            fail(f"themes/{name}.toml: dark must be a boolean")
        for table in ("ui", "doc", "syntax"):
            keys = set(t.get(table, {}))
            if table == "doc":  # per-level heading and bold / italic / quote colours are optional
                keys -= {f"heading_{i}" for i in range(1, 7)} | {"bold", "italic", "quote"}
            if keys != ref_keys[table]:
                fail(f"themes/{name}.toml [{table}]: missing {sorted(ref_keys[table] - keys)}, "
                     f"extra {sorted(keys - ref_keys[table])}")
            for k, v in t.get(table, {}).items():
                ok = parse_style(v) if table == "syntax" else norm_colour(v)
                if ok is None:
                    fail(f"themes/{name}.toml [{table}] {k} = {v!r} is not a valid value")
                if table == "syntax" and any(w not in ("bold", "italic") for w in v.split()[1:]):
                    fail(f"themes/{name}.toml [syntax] {k}: unknown style word in {v!r}")
        for k in THEME_TXT_ENTRIES:
            if THEME_TXT_ENTRIES[k] and k not in t.get("syntax", {}):
                fail(f"themes/{name}.toml [syntax] lacks theme.txt entry {k!r}")
        cs = theme_contrasts(t)
        rows.append((name, cs))
        for k, minimum in MINIMUMS.items():
            if cs[k] < minimum:
                msg = f"themes/{name}.toml: contrast {k} = {cs[k]:.2f} < {minimum}"
                if name in LEGACY:
                    print("warn:", msg)
                else:
                    fail(msg)
    if not quiet:
        cols = list(rows[0][1])
        print("\n" + "contrast".ljust(20) + "".join(f"{c:>11}" for c in cols))
        for name, cs in rows:
            print(f"{name:20}" + "".join(f"{cs[c]:11.2f}" for c in cols))
        print()


def main():
    regen = "--regen" in sys.argv
    quiet = "--quiet" in sys.argv

    check_themes(quiet)

    # All TOML inputs must parse, except the deliberately broken one.
    for p in sorted(HERE.glob("*.toml")):
        try:
            load_toml(p)
            if p.name == "config-broken.toml":
                fail("config-broken.toml unexpectedly parses")
        except tomllib.TOMLDecodeError as e:
            if p.name != "config-broken.toml":
                fail(f"{p.name} does not parse: {e}")

    for p in sorted(HERE.glob("config-*.toml")):
        resolved = resolve_config(p.read_bytes())
        check_file(p.with_suffix(".expected"), config_expected_text(resolved).encode(), regen)

    for p in sorted(HERE.glob("edit-*.toml")):
        ops = p.with_suffix(".ops")
        if not ops.exists():
            fail(f"{ops.name} is missing")
            continue
        result = run_edit(p.read_bytes(), ops.read_bytes())
        try:
            tomllib.loads(result.decode("utf-8"))
        except tomllib.TOMLDecodeError as e:
            fail(f"{p.stem}: the edited file is not valid TOML: {e}")
        # Every op's value must read back from the edited file.
        doc = tomllib.loads(result.decode("utf-8"))
        for dotted, value in parse_ops(ops.read_text(encoding="utf-8")):
            got = doc
            for part in dotted.split("."):
                got = got.get(part) if isinstance(got, dict) else None
            if got != value or type(got) is not type(value):
                fail(f"{p.stem}: {dotted} reads back as {got!r}, expected {value!r}")
        check_file(p.with_suffix(".expected-toml"), result, regen)

    for p in sorted(HERE.glob("theme-*.toml")):
        if p.name == "theme-syntax.toml":
            continue
        resolved = resolve_theme(load_toml(p))
        check_file(p.with_suffix(".expected"), theme_expected_text(resolved).encode(), regen)

    themes = dict(BUILTIN)
    extra = load_toml(HERE / "theme-syntax.toml")
    themes[extra["name"]] = extra
    lookup = HERE / "syntax-lookup.txt"
    out_lines = []
    for n, line in enumerate(lookup.read_text(encoding="utf-8").splitlines(), 1):
        m = re.fullmatch(r"(\S+) (\S+) => (.+)", line)
        if not m:
            out_lines.append(line)
            if line.strip() and not line.startswith("#"):
                fail(f"syntax-lookup.txt:{n}: malformed line {line!r}")
            continue
        theme_name, capture, want = m.groups()
        if theme_name not in themes:
            fail(f"syntax-lookup.txt:{n}: unknown theme {theme_name!r}")
            out_lines.append(line)
            continue
        got = lookup_syntax(themes[theme_name], capture)
        out_lines.append(f"{theme_name} {capture} => {got}")
        if not regen and got != want:
            fail(f"syntax-lookup.txt:{n}: {theme_name} {capture} => {want}, reference gives {got}")
    if regen:
        lookup.write_text("\n".join(out_lines) + "\n", encoding="utf-8", newline="\n")
        print("wrote", lookup.name)

    fontpick = sorted(HERE.glob("fontpick-*.txt"))
    if not fontpick:
        fail("no fontpick-*.txt files")
    for p in fontpick:
        check_fontpick(p, regen)

    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s)")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
