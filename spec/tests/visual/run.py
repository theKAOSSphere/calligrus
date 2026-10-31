#!/usr/bin/env python3
"""Visual regression suite for calligrus (spec section 20).

Runs every *.mdscript here headless (--script), compares the shots with
refs/<os>/<script>/<shot>.png and writes report/index.html (reference | new | diff heat-map).

  python run.py                   compare against the references
  python run.py --update          accept the current output as the new references
  python run.py --exe PATH        the calligrus executable to test (or $CALLIGRUS_EXE)
  python run.py --only math       only scripts/shots whose "script/shot" contains this
                                  (fnmatch pattern if it has * ? [ ])
Exit code 0 = every shot within tolerance.

The runs set CALLIGRUS_DISPLAY_ROOT to the repository folder, so the toolbar and the recent-files
popup show paths relative to it (spec/tests/visual/pages.md) and the shots are the same on every machine.

A script may carry a line "# config: FILE.toml" to use another config than config.toml.
"""
import argparse, fnmatch, html, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path
from PIL import Image, ImageChops

HERE = Path(__file__).resolve().parent
OS = "windows" if sys.platform.startswith("win") else "linux"
REPO = HERE.parents[2]          # the repository root (calligrus/)

def default_exe():
    if OS == "windows":
        return REPO / "build/Release/calligrus.exe"
    return REPO / "build/calligrus"

def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true", help="accept current output as references")
    ap.add_argument("--only", help="pattern on 'script/shot'")
    ap.add_argument("--tolerance", type=int, default=24, help="per-channel diff counted as different (default 24)")
    ap.add_argument("--ignore-top", type=int, default=0,
                    help="ignore the top N pixel rows against the references (not needed: the runs show paths relative to the repository)")
    ap.add_argument("--max-diff", type=float, default=0.1, help="max %% of differing pixels (default 0.1)")
    ap.add_argument("--exe", default=os.environ.get("CALLIGRUS_EXE"), help="the calligrus executable (default: build/Release/calligrus.exe or build/calligrus)")
    ap.add_argument("--timeout", type=int, default=180)
    return ap.parse_args()

def match(pattern, name):
    if not pattern:
        return True
    if any(c in pattern for c in "*?["):
        return fnmatch.fnmatch(name, pattern) or fnmatch.fnmatch(name, "*" + pattern + "*")
    return pattern in name

def script_config(script):
    for line in script.read_text(encoding="utf-8").splitlines():
        if line.startswith("# config:"):
            return HERE / line.split(":", 1)[1].strip()
    return HERE / "config.toml"

def run_script(exe, script, outdir, state, timeout):
    """Run one script; return (ok, message)."""
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    env = dict(os.environ, CALLIGRUS_CONFIG=str(script_config(script)), CALLIGRUS_STATE=str(state),
               CALLIGRUS_DISPLAY_ROOT=str(REPO))
    env.pop("CALLIGRUS_SCRIPT", None)
    cmd = [str(exe), "--script", str(script), "--out", str(outdir)]
    if OS == "linux":
        # Always an invisible Xvfb display: under WSL, DISPLAY/WAYLAND_DISPLAY point at
        # WSLg, which would show real windows on the Windows desktop.
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        cmd = ["xvfb-run", "-a"] + cmd
    state.write_text("", encoding="utf-8")   # fresh empty state every run
    try:
        p = subprocess.run(cmd, cwd=HERE, env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "timeout"
    finally:
        log = Path(str(script) + ".log")     # the apps append a log next to the script
        if log.exists():
            try: log.unlink()
            except OSError: pass
    if p.returncode != 0:
        return False, f"exit {p.returncode}: {(p.stderr or p.stdout).strip()[:400]}"
    return True, ""

def check_pdf_pages(script, outdir):
    """A script may carry lines "# expect-pdf-pages: FILE.pdf N": that PDF in the output folder must have
    N pages (counted as /Type /Page not followed by s). Returns an error message or ""."""
    import re
    for line in script.read_text(encoding="utf-8").splitlines():
        if line.startswith("# expect-pdf-pages:"):
            name, want = line.split(":", 1)[1].split()
            pdf = outdir / name
            if not pdf.exists():
                return f"{name} was not written"
            got = len(re.findall(rb"/Type\s*/Page(?!s)", pdf.read_bytes()))
            if got != int(want):
                return f"{name} has {got} pages, expected {want}"
    return ""

def check_pdf_text(script, outdir, exe, state):
    """Checks of a PDF with real text (spec 33), from lines in the script (skipped when PyMuPDF is missing):
      # expect-pdf-text: FILE.pdf word|another word    its text (PyMuPDF get_text) has all of these
      # expect-pdf-max-kb: FILE.pdf N                  the file is at most N KB
      # expect-pdf-no-repeat: FILE.pdf                 no page repeats the end of the page before it (its last text lines
                                                       are not among the next page's first ones) and no word sticks out
                                                       above or below its page (text cut by a page's edge belongs elsewhere)
      # expect-pdf-raster-diff: FILE.pdf DOC.md PCT    page 1, rendered by PyMuPDF at the export's dpi (144), differs
                                                       in at most PCT % of its pixels (by more than 32 levels) from page 1 of
                                                       the same document exported with --raster (other rasterisers: a tolerance)
    Returns an error message or ""."""
    lines = [l for l in script.read_text(encoding="utf-8").splitlines() if l.startswith("# expect-pdf-")
             and not l.startswith("# expect-pdf-pages:")]
    if not lines:
        return ""
    import re
    try:
        import fitz
    except ImportError:
        print("  (PyMuPDF missing: the PDF text checks are skipped)")
        return ""
    for line in lines:
        key, rest = line[2:].split(":", 1)
        parts = rest.strip().split(None, 1) if key != "expect-pdf-raster-diff" else rest.strip().split()
        pdf = outdir / parts[0]
        if not pdf.exists():
            return f"{parts[0]} was not written"
        if key == "expect-pdf-max-kb":
            kb = pdf.stat().st_size / 1024
            if kb > float(parts[1]):
                return f"{parts[0]} is {kb:.0f} KB, more than {parts[1]} KB"
        elif key == "expect-pdf-text":
            text = " ".join(fitz.open(pdf).load_page(i).get_text() for i in range(len(fitz.open(pdf)))).split()
            text = " ".join(text)
            for w in parts[1].split("|"):
                if " ".join(w.split()) not in text:
                    return f"{parts[0]} has no text {w!r}"
        elif key == "expect-pdf-no-repeat":
            doc = fitz.open(pdf)
            def content(page):  # the page's text lines, without its footer ("2 / 3") and tiny ones
                ls = [" ".join(l.split()) for l in page.get_text().splitlines()]
                return [l for l in ls if len(l) > 3 and not re.fullmatch(r"\d+ / \d+", l)]
            for i, page in enumerate(doc):
                for w in page.get_text("words"):
                    if w[1] < 0 or w[3] > page.rect.height:
                        return f"{parts[0]} page {i + 1}: {w[4]!r} sticks out of the page (y {w[1]:.0f}..{w[3]:.0f})"
                if i > 0:
                    again = set(content(doc[i - 1])[-3:]) & set(content(page)[:3])
                    if again:
                        return f"{parts[0]} page {i + 1} starts with text from the end of page {i}: {sorted(again)[0]!r}"
        elif key == "expect-pdf-raster-diff":
            raster = outdir / "raster-for-diff.pdf"
            env = dict(os.environ, CALLIGRUS_CONFIG=str(script_config(script)), CALLIGRUS_STATE=str(state),
                       CALLIGRUS_DISPLAY_ROOT=str(REPO))
            env.pop("CALLIGRUS_SCRIPT", None)
            cmd = [str(exe), "--export", str(raster), "--pages", str(HERE / parts[1]), "--raster"]
            if OS == "linux":
                env.pop("DISPLAY", None); env.pop("WAYLAND_DISPLAY", None)
                cmd = ["xvfb-run", "-a"] + cmd
            r = subprocess.run(cmd, cwd=HERE, env=env, capture_output=True, text=True, timeout=120)
            if r.returncode != 0 or not raster.exists():
                return f"the --raster export failed: {(r.stderr or r.stdout).strip()[:200]}"
            def page1(f):
                px = fitz.open(f).load_page(0).get_pixmap(dpi=144)
                return Image.frombytes("RGB", (px.width, px.height), px.samples)
            a, b = page1(pdf), page1(raster)
            if a.size != b.size:
                return f"page 1 is {a.size} but the raster export's is {b.size}"
            d = ImageChops.difference(a, b)
            mx = ImageChops.lighter(ImageChops.lighter(*d.split()[:2]), d.split()[2])
            pct = 100.0 * sum(mx.histogram()[33:]) / (a.size[0] * a.size[1])
            if pct > float(parts[2]):
                return f"{parts[0]} page 1 differs from the raster export in {pct:.1f} % of its pixels (limit {parts[2]} %)"
    return ""

def script_masks(script):
    """Lines "# mask: X0 Y0 X1 Y1" give rectangles left out of every comparison of that script's shots
    (for machine-specific content, such as folder paths in the recent-files popup)."""
    out = []
    for line in script.read_text(encoding="utf-8").splitlines():
        if line.startswith("# mask:"):
            out.append(tuple(int(v) for v in line.split(":", 1)[1].split()))
    return out

def compare(a_path, b_path, tol, top=0, masks=()):
    """Return (fraction_of_differing_pixels, diff_image or None, size_mismatch).
    top > 0 leaves out the top `top` pixel rows of both images; masks are rectangles painted
    the same gray in both before comparing."""
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        return 1.0, None, f"size {a.size[0]}x{a.size[1]} vs {b.size[0]}x{b.size[1]}"
    for m in masks:
        a.paste((128, 128, 128), m)
        b.paste((128, 128, 128), m)
    if 0 < top < a.size[1]:
        a = a.crop((0, top, a.size[0], a.size[1]))
        b = b.crop((0, top, b.size[0], b.size[1]))
    d = ImageChops.difference(a, b)
    r, g, bl = d.split()
    mx = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    hist = mx.histogram()
    n = a.size[0] * a.size[1]
    bad = sum(hist[tol + 1:])
    if sum(hist[1:]) == 0:
        return 0.0, None, ""
    return bad / n, heat(b, mx, tol), ""

def heat(base, mx, tol):
    """Dimmed grayscale of the new image, orange where slightly different, red where beyond tolerance."""
    gray = base.convert("L").point(lambda v: 150 + v // 3).convert("RGB")
    small = mx.point(lambda v: 255 if 0 < v <= tol else 0)
    big = mx.point(lambda v: 255 if v > tol else 0)
    gray.paste((255, 170, 0), mask=small)
    gray.paste((230, 0, 0), mask=big)
    return gray

def collect_shots(d):
    return sorted(p.name for p in d.glob("*.png")) if d.exists() else []

def main():
    args = parse_args()
    exe = Path(args.exe) if args.exe else default_exe()
    scripts = sorted(HERE.glob("*.mdscript"))
    if args.only:
        head = args.only.split("/")[0]
        scripts = [s for s in scripts if match(args.only, s.stem) or match(head, s.stem)]
    if not exe.exists():
        print(f"exe not found: {exe}")
        return 1

    outroot = HERE / "out"
    refroot = HERE / "refs" / OS
    repdir = HERE / "report"
    (repdir / "diff").mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix="calligrus-visual-"))
    state = tmp / "state.toml"

    rows = []      # one dict per shot: script, shot, status, pct, note, ref, new, diff
    errors = []
    for s in scripts:
        t0 = time.time()
        outdir = outroot / s.stem
        ok = False
        for attempt in range(3):          # exe may be briefly locked/rebuilt by someone else
            ok, msg = run_script(exe, s, outdir, state, args.timeout)
            # Retry timeouts, and the occasional failed GL start right after Xvfb comes up.
            if ok or ("exit" in msg and "initialization failed" not in msg):
                break
            time.sleep(5)
        if ok:
            msg = check_pdf_pages(s, outdir) or check_pdf_text(s, outdir, exe, state)
            ok = not msg
        if not ok:
            print(f"{s.stem}: FAILED {msg}")
            errors.append((s.stem, msg))
            rows.append(dict(script=s.stem, shot="(script)", status="error", pct=1.0, note=msg))
            continue
        shots = collect_shots(outdir)
        shots = [x for x in shots if match(args.only, f"{s.stem}/{x}") or match(args.only, s.stem)]
        refdir = refroot / s.stem
        if args.update:
            if refdir.exists():
                shutil.rmtree(refdir)
            refdir.mkdir(parents=True)
            for x in shots:
                shutil.copy2(outdir / x, refdir / x)
        bad = 0
        for x in shots:
            new = outdir / x
            ref = refdir / x
            row = dict(script=s.stem, shot=x, ref=ref, new=new)
            if not ref.exists():
                row.update(status="no-ref", pct=1.0, note="no reference (run with --update)")
            else:
                frac, dimg, note = compare(ref, new, args.tolerance, args.ignore_top, script_masks(s))
                row.update(pct=frac * 100, note=note)
                row["status"] = "pass" if (not note and frac * 100 <= args.max_diff) else "fail"
                if dimg is not None:
                    dp = repdir / "diff" / s.stem / x
                    dp.parent.mkdir(parents=True, exist_ok=True)
                    dimg.save(dp)
                    row["diff"] = dp
            if row["status"] != "pass":
                bad += 1
            rows.append(row)
        for x in collect_shots(refdir):          # references that were not produced any more
            if x not in shots and not args.only:
                rows.append(dict(script=s.stem, shot=x, status="missing", pct=1.0, note="shot not produced"))
                bad += 1
        print(f"{s.stem:<22} {len(shots):>2} shots  {'ok' if not bad else str(bad) + ' BAD'}  ({time.time()-t0:.1f}s)")

    write_report(repdir, rows, args)

    for p in tmp.glob("*"):
        p.unlink()
    tmp.rmdir()

    n_bad = sum(1 for r in rows if r["status"] != "pass")
    print(f"\n{len(rows) - n_bad}/{len(rows)} shots ok; report: {repdir / 'index.html'}")
    return 0 if n_bad == 0 and not errors and rows else 1

def rel(p, base):
    return os.path.relpath(p, base).replace("\\", "/")

def img(p, base, cls=""):
    if not p:
        return "<span class=na>-</span>"
    r = rel(p, base)
    return f'<a href="{r}"><img loading=lazy class="{cls}" src="{r}"></a>'

def write_report(repdir, rows, args):
    esc = html.escape
    out = ["<!doctype html><meta charset=utf-8><title>calligrus visual report</title><style>"
           "body{font:13px sans-serif;margin:16px;background:#fafafa}table{border-collapse:collapse;margin-bottom:24px}"
           "td,th{border:1px solid #ccc;padding:4px;vertical-align:top}img{width:300px;display:block;border:1px solid #ddd}"
           ".pass{background:#e6f7e6}.fail,.error,.missing,.no-ref{background:#fde4e4}.na{color:#999}"
           "h2{margin-top:32px}.hd{font-weight:bold}</style>",
           f"<h1>calligrus visual regression ({OS})</h1><p>tolerance {args.tolerance}, max differing pixels {args.max_diff}%. "
           "Heat map: red = beyond tolerance, orange = small difference.</p>"]
    bad = [r for r in rows if r["status"] != "pass"]
    out.append(f"<h2>{len(rows) - len(bad)}/{len(rows)} ok</h2><table><tr><th>shot<th>status<th>reference<th>new<th>diff")
    for r in sorted(rows, key=lambda r: (r["status"] == "pass", r["script"], r["shot"])):
        out.append(f"<tr class={r['status']}><td class=hd>{esc(r['script'])}/<br>{esc(r['shot'])}"
                   f"<td>{r['status']}<br>{r['pct']:.3f}%<br>{esc(r.get('note',''))}"
                   f"<td>{img(r.get('ref') if r.get('ref') and Path(r['ref']).exists() else None, repdir)}"
                   f"<td>{img(r.get('new'), repdir)}<td>{img(r.get('diff'), repdir)}")
    out.append("</table>")
    (repdir / "index.html").write_text("\n".join(out), encoding="utf-8")

if __name__ == "__main__":
    sys.exit(main())
