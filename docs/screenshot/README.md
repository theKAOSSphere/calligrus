# README screenshot

`hero.png` (used at the top of the main README) is rendered headlessly by Calligrus itself from
`showcase.md`, with the theme and settings in `config.toml`, so it can be remade after any change.

From this folder, after a build (Windows: `../../build/Release/calligrus.exe`; Linux: `../../build/calligrus`):

```sh
CALLIGRUS_CONFIG="$PWD/config.toml" CALLIGRUS_STATE="$(mktemp)" CALLIGRUS_DISPLAY_ROOT="$PWD" \
    ../../build/Release/calligrus.exe --script hero.mdscript --out .
```

`CALLIGRUS_DISPLAY_ROOT` makes the toolbar show `showcase.md` instead of the full folder path.

## Editor screenshot

`editor-demo.org` is the file for a by-hand screenshot of Emacs (or Neovim) beside the Calligrus preview. Its
wide table with long notes runs off-screen in the editor and wraps cleanly in the preview. Copy it into a neutral
folder first, so the editor's mode line and title bar show no personal paths.
