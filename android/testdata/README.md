# Android test documents

Documents for checking the Android app by hand on a device or emulator (push them, then open them from the file picker).

- `gujarati-table.md` — a heading and a two-column table with long Gujarati and English text: system-font fallback for Gujarati, wrapping and sideways table scrolling.
- `code-paths.md` — inline code full of long paths, arrows and ligature-like sequences, next to text that needs a system font: inline-code spacing and wrapping, and code drawn while a fallback font is in use.
- `gen-large-doc.py` — writes a large document (`python3 gen-large-doc.py > find-large.md`, about 3000 lines) for find and scrolling speed.
