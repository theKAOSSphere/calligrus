#!/usr/bin/env python3
"""Re-create the vendored grammar tree in this folder from upstream checkouts.

Usage:
    python vendor.py <sources-dir> [--runtime <tree-sitter-crate-dir>]

<sources-dir> holds a git checkout of every grammar repo listed in LANGS
(folder name = repo name), plus `helix/latex.scm` and `helix/LICENSE` (Helix's LaTeX query, MPL-2.0).
--runtime points at the unpacked `tree-sitter` crate (e.g. tree-sitter-0.27.0
from ~/.cargo/registry) whose C runtime is copied to runtime/ for the C++ build.

What it does, per language:
  * copies src/parser.c, src/scanner.c (if any), the src/tree_sitter/*.h
    headers and any extra file the scanner #includes, plus the licence;
  * builds ONE final `highlights.scm` from the upstream query files (resolving
    inheritance such as C++ -> C and TypeScript -> JavaScript by concatenation),
    then normalises it so both apps can evaluate it identically:
      - drops predicates/directives other than the text predicates both apps
        implement (#eq? #match? #any-of? and their not-/any- variants), e.g.
        #set!, #is-not? local, Neovim's #lua-match? (converted to #match?);
      - rewrites regexes into a small common subset (ASCII only: \\d -> [0-9],
        no lookaround, ...) and refuses anything outside it, so Rust's `regex`
        crate and C++'s std::regex (ECMAScript) agree on every pattern;
      - removes Neovim's @spell / @nospell marker captures.
Pattern order is kept: all these sources are written for "the later pattern
wins" (as in tree-sitter-highlight, Neovim and Helix), which is also calligrus's
rule. (Checked against the upstream test/highlight assertion files: reversing
the order made every language score worse or equal.)
The result is checked by the golden tests of both apps.
"""

import argparse
import os
import re
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))

# name, repo folder, grammar subfolder, query files (relative to sources,
# concatenated in this order).
LANGS = [
    ("rust", "tree-sitter-rust", ".", ["tree-sitter-rust/queries/highlights.scm"]),
    ("c", "tree-sitter-c", ".", ["tree-sitter-c/queries/highlights.scm"]),
    # Order as in tree-sitter-cpp's tree-sitter.json: C's patterns, then C++'s.
    ("cpp", "tree-sitter-cpp", ".", ["tree-sitter-c/queries/highlights.scm", "tree-sitter-cpp/queries/highlights.scm"]),
    ("python", "tree-sitter-python", ".", ["tree-sitter-python/queries/highlights.scm"]),
    ("javascript", "tree-sitter-javascript", ".", [
        "tree-sitter-javascript/queries/highlights.scm",
        "tree-sitter-javascript/queries/highlights-jsx.scm",
        "tree-sitter-javascript/queries/highlights-params.scm"]),
    # Order as in tree-sitter-typescript's tree-sitter.json (TS first, then JS).
    ("typescript", "tree-sitter-typescript", "typescript", [
        "tree-sitter-typescript/queries/highlights.scm",
        "tree-sitter-javascript/queries/highlights.scm"]),
    ("tsx", "tree-sitter-typescript", "tsx", [
        "tree-sitter-typescript/queries/highlights.scm",
        "tree-sitter-javascript/queries/highlights-jsx.scm",
        "tree-sitter-javascript/queries/highlights.scm"]),
    ("json", "tree-sitter-json", ".", ["tree-sitter-json/queries/highlights.scm"]),
    ("toml", "tree-sitter-toml", ".", ["tree-sitter-toml/queries/highlights.scm"]),
    ("yaml", "tree-sitter-yaml", ".", ["tree-sitter-yaml/queries/highlights.scm"]),
    ("bash", "tree-sitter-bash", ".", ["tree-sitter-bash/queries/highlights.scm"]),
    ("powershell", "tree-sitter-powershell", ".", ["tree-sitter-powershell/queries/highlights.scm"]),
    ("lua", "tree-sitter-lua", ".", ["tree-sitter-lua/queries/highlights.scm"]),
    ("elisp", "tree-sitter-elisp", ".", ["tree-sitter-elisp/queries/highlights.scm"]),
    ("markdown", "tree-sitter-markdown", "tree-sitter-markdown", ["tree-sitter-markdown/tree-sitter-markdown/queries/highlights.scm"]),
    ("latex", "tree-sitter-latex", ".", ["helix/latex.scm"]),
    ("cmake", "tree-sitter-cmake", ".", ["tree-sitter-cmake/queries/highlights.scm"]),
    ("go", "tree-sitter-go", ".", ["tree-sitter-go/queries/highlights.scm"]),
    ("html", "tree-sitter-html", ".", ["tree-sitter-html/queries/highlights.scm"]),
    ("css", "tree-sitter-css", ".", ["tree-sitter-css/queries/highlights.scm"]),
    ("autohotkey", "tree-sitter-autohotkey", ".", ["tree-sitter-autohotkey/queries/highlights.scm"]),
]

# Injection queries copied verbatim (they only use @injection.content and
# `#set! injection.language`, which both apps interpret).
INJECTIONS = {"html": "tree-sitter-html/queries/injections.scm"}

# Extra files a scanner #includes, and include lines rewritten to match.
EXTRA_FILES = {
    "html": [("src/tag.h", "src/tag.h")],
    "yaml": [("src/schema.core.c", "src/schema.core.c")],
    "typescript": [("../common/scanner.h", "src/common_scanner.h")],
    "tsx": [("../common/scanner.h", "src/common_scanner.h")],
}
INCLUDE_PATCHES = {
    "typescript": ('#include "../../common/scanner.h"', '#include "common_scanner.h"'),
    "tsx": ('#include "../../common/scanner.h"', '#include "common_scanner.h"'),
}

KEEP_PREDICATES = {
    "eq?", "not-eq?", "any-eq?", "any-not-eq?",
    "match?", "not-match?", "any-match?", "any-not-match?",
    "any-of?", "not-any-of?",
}
DROP_CAPTURES = {"spell", "nospell"}


# --- query tokenizer -----------------------------------------------------------

TOKEN_RE = re.compile(r'''
      (?P<comment>;[^\n]*)
    | (?P<string>"(?:[^"\\]|\\.)*")
    | (?P<open>[(\[])
    | (?P<close>[)\]])
    | (?P<space>\s+)
    | (?P<atom>[^\s()\[\]";]+)
''', re.VERBOSE | re.DOTALL)


def tokenize(text):
    toks = []
    pos = 0
    while pos < len(text):
        m = TOKEN_RE.match(text, pos)
        if not m:
            raise SystemExit(f"cannot tokenize query at {text[pos:pos+40]!r}")
        toks.append((m.lastgroup, m.group(), m.start(), m.end()))
        pos = m.end()
    return toks


def matching_close(toks, i):
    depth = 0
    for j in range(i, len(toks)):
        if toks[j][0] == "open":
            depth += 1
        elif toks[j][0] == "close":
            depth -= 1
            if depth == 0:
                return j
    raise SystemExit("unbalanced parentheses in query")


def next_meaningful(toks, i):
    while i < len(toks) and toks[i][0] in ("space", "comment"):
        i += 1
    return i


# --- regex normalisation --------------------------------------------------------

LUA_CLASSES = {"u": "A-Z", "l": "a-z", "d": "0-9", "a": "A-Za-z", "w": "A-Za-z0-9", "s": " \\t\\n\\r"}


def lua_to_regex(pat):
    """Convert the simple Lua patterns used by Neovim queries to our regex subset."""
    out = []
    i = 0
    in_class = False
    while i < len(pat):
        c = pat[i]
        if c == "%" and i + 1 < len(pat):
            n = pat[i + 1]
            if n in LUA_CLASSES:
                cls = LUA_CLASSES[n]
                out.append(cls if in_class else f"[{cls}]")
            else:
                out.append("\\" + n)
            i += 2
            continue
        if c == "[":
            in_class = True
        elif c == "]":
            in_class = False
        out.append(c)
        i += 1
    return "".join(out)


def normalise_regex(rx, where):
    """Rewrite into the shared subset and validate it. `rx` is the regex text
    (already unescaped from the query string)."""
    out = []
    i = 0
    in_class = False
    while i < len(rx):
        c = rx[i]
        if c == "\\":
            if i + 1 >= len(rx):
                raise SystemExit(f"{where}: dangling backslash in regex {rx!r}")
            n = rx[i + 1]
            if n == "d":
                out.append("0-9" if in_class else "[0-9]")
            elif n == "w":
                out.append("A-Za-z0-9_" if in_class else "[A-Za-z0-9_]")
            elif n == "s":
                out.append(" \\t\\n\\r" if in_class else "[ \\t\\n\\r]")
            elif n in "tnr":
                out.append("\\" + n)
            elif not n.isalnum():
                out.append("\\" + n)  # escaped punctuation: same in both flavours
            else:
                raise SystemExit(f"{where}: unsupported escape \\{n} in regex {rx!r}")
            i += 2
            continue
        if ord(c) > 127:
            raise SystemExit(f"{where}: non-ASCII character in regex {rx!r}")
        if in_class:
            if c == "]":
                in_class = False
            elif c == "[":
                raise SystemExit(f"{where}: nested class in regex {rx!r}")
        else:
            if c == "[":
                in_class = True
            elif c == "(" and rx[i + 1:i + 2] == "?":
                raise SystemExit(f"{where}: groups with (? flags/lookaround are not allowed: {rx!r}")
            elif c == "{":
                raise SystemExit(f"{where}: counted repetition is not allowed: {rx!r}")
            elif c == ".":
                # `.` counts characters in Rust but bytes in std::regex: only
                # allow it where the count doesn't matter.
                if rx[i + 1:i + 2] not in ("*", "+"):
                    raise SystemExit(f"{where}: '.' must be followed by * or + : {rx!r}")
        out.append(c)
        i += 1
    if in_class:
        raise SystemExit(f"{where}: unterminated class in regex {rx!r}")
    res = "".join(out)
    if "[^" in res:
        # same byte-vs-character concern as '.'
        for m in re.finditer(r"\[\^(?:[^\]\\]|\\.)*\]", res):
            if res[m.end():m.end() + 1] not in ("*", "+"):
                raise SystemExit(f"{where}: negated class must be followed by * or + : {rx!r}")
    return res


def unquote(s):
    """Query string literal -> value (tree-sitter supports \\n \\t \\r \\0 \\\\ \\")."""
    body = s[1:-1]
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            out.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def quote(v):
    return '"' + v.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'


# --- query transformation -------------------------------------------------------

def clean_query(text, where):
    toks = tokenize(text)
    edits = []  # (start, end, replacement)
    i = 0
    while i < len(toks):
        kind, val, s, e = toks[i]
        if kind == "open" and val == "(":
            j = next_meaningful(toks, i + 1)
            if j < len(toks) and toks[j][0] == "atom" and toks[j][1].startswith("#"):
                close = matching_close(toks, i)
                name = toks[j][1][1:]
                args = [t for t in toks[j + 1:close] if t[0] not in ("space", "comment")]
                if name == "lua-match?":
                    cap, pat = args[0][1], unquote(args[1][1])
                    rx = normalise_regex(lua_to_regex(pat), where)
                    edits.append((s, toks[close][3], f"(#match? {cap} {quote(rx)})"))
                elif name in ("match?", "not-match?", "any-match?", "any-not-match?"):
                    cap, rxs = args[0][1], args[1][1]
                    rx = normalise_regex(unquote(rxs), where)
                    edits.append((args[1][2], args[1][3], quote(rx)))
                elif name not in KEEP_PREDICATES:
                    edits.append((s, toks[close][3], ""))
                i = close + 1
                continue
        if kind == "atom" and val.startswith("@") and val[1:] in DROP_CAPTURES:
            edits.append((s, e, ""))
        i += 1
    for s, e, rep in sorted(edits, reverse=True):
        text = text[:s] + rep + text[e:]
    return text


def build_query(files, src_root):
    """Concatenate the query files (in order) and clean the result."""
    parts = []
    for f in files:
        with open(os.path.join(src_root, f), encoding="utf-8") as fh:
            parts.append(fh.read().rstrip("\n") + "\n")
    return clean_query("\n".join(parts), files[-1])


# --- driver ---------------------------------------------------------------------

def git_info(path):
    def run(*args):
        return subprocess.run(["git", "-C", path, *args], capture_output=True, text=True).stdout.strip()
    url = run("remote", "get-url", "origin")
    return url.removesuffix(".git"), run("rev-parse", "HEAD")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sources")
    ap.add_argument("--runtime")
    args = ap.parse_args()
    src = os.path.abspath(args.sources)

    provenance = []
    for name, repo, sub, queries in LANGS:
        repo_dir = os.path.join(src, repo)
        gdir = os.path.join(repo_dir, sub)
        dest = os.path.join(HERE, name)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.makedirs(os.path.join(dest, "src", "tree_sitter"))
        for f in ("parser.c", "scanner.c"):
            p = os.path.join(gdir, "src", f)
            if os.path.exists(p):
                shutil.copy(p, os.path.join(dest, "src", f))
        hdrs = os.path.join(gdir, "src", "tree_sitter")
        for h in os.listdir(hdrs):
            shutil.copy(os.path.join(hdrs, h), os.path.join(dest, "src", "tree_sitter", h))
        for frm, to in EXTRA_FILES.get(name, []):
            shutil.copy(os.path.normpath(os.path.join(gdir, frm)), os.path.join(dest, to))
        if name in INCLUDE_PATCHES:
            old, new = INCLUDE_PATCHES[name]
            p = os.path.join(dest, "src", "scanner.c")
            with open(p, encoding="utf-8", newline="") as fh:
                t = fh.read()
            assert old in t, (name, old)
            with open(p, "w", encoding="utf-8", newline="") as fh:
                fh.write(t.replace(old, new))
        lic = next(f for f in os.listdir(repo_dir) if f.upper().startswith("LICENSE"))
        shutil.copy(os.path.join(repo_dir, lic), os.path.join(dest, "LICENSE"))

        q = build_query(queries, src)
        header = [f"; Vendored for calligrus by vendor.py from: {', '.join(queries)}",
                  "; Precedence: for the same node, the LAST matching pattern wins."]
        if name == "latex":
            header.append("; Source: Helix (https://github.com/helix-editor/helix, MPL-2.0).")
            # Helix's LICENSE (MPL-2.0), expected next to latex.scm in <sources>/helix/.
            shutil.copy(os.path.join(src, "helix", "LICENSE"), os.path.join(dest, "LICENSE-highlights-helix"))
        with open(os.path.join(dest, "highlights.scm"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(header) + "\n\n" + q)
        if name in INJECTIONS:
            shutil.copy(os.path.join(src, INJECTIONS[name]), os.path.join(dest, "injections.scm"))
        url, commit = git_info(repo_dir)
        provenance.append((name, url, commit))

    if args.runtime:
        rt = os.path.join(HERE, "runtime")
        if os.path.isdir(rt):
            shutil.rmtree(rt)
        shutil.copytree(os.path.join(args.runtime, "src"), os.path.join(rt, "src"),
                        ignore=shutil.ignore_patterns("wasm-stdlib"))
        shutil.copytree(os.path.join(args.runtime, "include"), os.path.join(rt, "include"))
        shutil.copy(os.path.join(args.runtime, "LICENSE"), os.path.join(rt, "LICENSE"))

    for name, url, commit in provenance:
        print(f"{name:12} {url} {commit}")


if __name__ == "__main__":
    main()
