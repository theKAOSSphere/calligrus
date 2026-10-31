# Visual regression suite (spec section 20)

Each `*.mdscript` renders a few shots of one feature area, headless (`--script`). `run.py`
compares them with the references in `refs/<os>/<script>/<shot>.png` and writes
`report/index.html` (reference | new | diff heat-map).

    python run.py                 # compare (exit 0 = all within tolerance)
    python run.py --update        # accept the current output as references
    python run.py --exe PATH      # the Calligrus executable to test
    python run.py --only math     # substring / fnmatch on "script/shot"
    python run.py --tolerance 24 --max-diff 0.1

Exe default: `build/Release/calligrus.exe` (Windows) or `build/calligrus` (Linux; always run
under `xvfb-run -a` with `DISPLAY`/`WAYLAND_DISPLAY` removed, so no window is ever shown), or
`--exe` / `CALLIGRUS_EXE`. References are per OS (`windows`, `linux`); `refs/linux` is made
on a Linux machine with `python3 run.py --update`.

The runs set `CALLIGRUS_DISPLAY_ROOT` to the repository folder: the toolbar, the recent-files popup
and tab tooltips then show paths relative to it (`spec/tests/visual/pages.md`), so the shots do
not depend on where the repository is checked out. (`--ignore-top N` still exists to leave the top
N pixel rows out of the comparison, but nothing needs it any more.)

Runs use `config.toml` (calligrus-light, zoom 1, outline hidden) via `CALLIGRUS_CONFIG`
and an empty temporary state file via `CALLIGRUS_STATE`. A script can start with
`# config: config-nord.toml` to use another config (themes, outline, line numbers; `config-indic.toml`
loads the test font `../fonts/NotoSansDevanagari-Regular.ttf`, a path relative to this folder).
Lines `# mask: X0 Y0 X1 Y1` leave a rectangle out of the comparison of every shot of that script
(for content that differs between machines; no script needs one at present).
`out/` holds the latest shots; `report/` the latest report (both can be deleted).
