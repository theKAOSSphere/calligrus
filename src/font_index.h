// Fonts by family name (spec 8a): an index of the installed system fonts, and
// the rules that pick a family's regular / bold / italic / bold-italic faces.
//
// A [fonts] value in config.toml is either a file path or a *family name*
// ("Inter", "Cascadia Code"). Names are looked up in the index, which is built
// by reading the `name`, `OS/2` and `post` tables of every font file in the
// system font folders (our own small sfnt reader, no FreeType). Scanning takes
// up to about a second, so it runs on a background thread at start-up; until
// it is done, named fonts can't be resolved yet (the app uses the built-in
// faces and switches when the index is ready). The result is saved (with each face's character coverage) in a
// cache file next to the app's state, keyed by every font file's path, size and modification time: the next start
// reads only the files that are new or changed (spec 30).
//
// The picking rules (pick_faces) are separate from the scanner so the shared
// tests (the Rust port) can feed them a fake index.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.h"  // Config::Fonts
#include "fonts.h"   // FontSource / FontFiles

// One face of a font file (a .ttc collection holds several).
struct FontFace {
    std::string family;     // typographic family (name ID 16), else the family (ID 1)
    std::string subfamily;  // typographic subfamily (ID 17), else the subfamily (ID 2)
    int weight = 400;       // OS/2 usWeightClass (100 thin .. 400 regular .. 700 bold .. 900 black)
    bool italic = false;    // OS/2 fsSelection bit 0, or "Italic"/"Oblique" in the subfamily
    bool mono = false;      // post.isFixedPitch != 0, or OS/2 panose proportion == 9
    std::string file;       // UTF-8 path
    int face = 0;           // index of the face inside a .ttc (0 for .ttf/.otf)
    int width = 5;          // OS/2 usWidthClass, 1..9 (5 = normal; missing / out of range -> 5)

    bool operator==(const FontFace&) const = default;
};

// Is a [fonts] value a family name? Yes unless it contains '/' or '\' or ends
// in .ttf / .otf / .ttc (any case). "" is neither (= built-in).
bool is_font_family_name(std::string_view value);

// Case-insensitive (ASCII) comparison of family names.
bool same_family(std::string_view a, std::string_view b);

// The faces picked for a family. A style with no suitable face is nullopt
// (the caller then uses the family's regular face, spec 5 rule).
struct FacePicks {
    std::optional<FontFace> regular, bold, italic, bold_italic;
};

// Pick a family's faces from `faces` (spec 8a; exact rules: spec/tests/README.md §5):
//   regular     = upright face with weight closest to 400;
//   bold        = upright face closest to 700 among weights >= 600;
//   italic      = italic face closest to 400;
//   bold-italic = italic face closest to 700 among weights >= 600.
// Width first (spec 8a "Width"): among a style's candidates, the faces whose
// width class is closest to 5 (normal) win, so "Arial" doesn't pick Arial
// Narrow; then the weight rule above.
// Ties: the lower weight, then the file path (UTF-8 bytes), then the face
// index as a number. A family with no upright face uses its italic pick as
// regular. Family names match exactly, ASCII case-insensitively.
// No face of that family at all -> all nullopt (so `regular` set = family known).
FacePicks pick_faces(const std::vector<FontFace>& faces, std::string_view family);

// Italic for picking: the italic bit, or "italic"/"oblique" (ASCII any case) in the subfamily.
bool is_italic_face(const FontFace& f);

// The faces of one font file (every face of a .ttc), read from its tables.
// Empty if the file can't be read or isn't a TrueType/OpenType font.
std::vector<FontFace> read_font_faces(const std::filesystem::path& file);

// Same, from the file's bytes in memory (`file` is only stored in the result).
std::vector<FontFace> parse_font_faces(std::string_view data, const std::string& file);

// The `cmap` table of face `face` of a font file, and whether that face is a colour font
// (COLR, CBDT, sbix or SVG table: colour emoji). Reads only the table directory and the cmap, not
// the whole file. False if the file can't be read or has no cmap. (For sysfont.h, spec 30.)
bool read_font_cmap(const std::filesystem::path& file, int face, std::string& cmap, bool& colour);

// Which characters a face has: sorted, merged, inclusive ranges of code points (what its Unicode cmap subtables map
// to a glyph), and whether it is a colour font. Kept in the index, and saved with it (spec 30), so choosing a system
// font for missing characters needs no font file at all.
struct FaceCoverage {
    bool colour = false;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    bool has(char32_t c) const;
};

// The code points a `cmap` table maps to a glyph (the Unicode subtables of format 4, 6, 12 and 13), as ranges.
std::vector<std::pair<uint32_t, uint32_t>> cmap_ranges(std::string_view cmap);

// A family as listed in the settings dropdown.
struct FontFamily {
    std::string name;   // as the regular pick's file spells it
    bool mono = false;  // any face of the family is monospaced
};

// The index of installed fonts, built on a background thread.
//
// Thread safety: start() launches the scan; ready() is an atomic flag; faces()
// and families() may only be used once ready() returned true (the scan never
// touches them again after that).
class FontIndex {
public:
    // Scan `dirs` (default: system_font_dirs()) in the background. `on_ready`
    // is called from the scanning thread when done (e.g. to wake the UI loop).
    // `cache`: the file the index is saved to and read from (default, for the system's folders only:
    // default_cache_file(); with `dirs` given and no `cache`, nothing is saved).
    void start(std::function<void()> on_ready = {}, std::vector<std::filesystem::path> dirs = {}, std::filesystem::path cache = {});
    bool ready() const { return state_ && state_->ready.load(std::memory_order_acquire); }
    // Wait up to `seconds` for the scan to finish; returns ready().
    bool wait(double seconds) const;

    const std::vector<FontFace>& faces() const;      // sorted by file path, then face index
    const std::vector<FontFamily>& families() const;  // sorted by name (case-insensitive), unique
    // The characters of a face of the index (nullptr: unknown, or not ready). Same rule as faces().
    const FaceCoverage* coverage(const FontFace& face) const;

    // Where the index is saved: <state folder>/calligrus/fontindex.cache ($XDG_STATE_HOME or ~/.local/state; on
    // Android the app's files folder; on Windows %LOCALAPPDATA%). Empty if there is no such folder.
    static std::filesystem::path default_cache_file();

    // The folders scanned on this system: %WINDIR%\Fonts and
    // %LOCALAPPDATA%\Microsoft\Windows\Fonts (per-user installs).
    static std::vector<std::filesystem::path> system_font_dirs();

private:
    struct State {
        std::atomic<bool> ready{false};
        std::vector<FontFace> faces;
        std::vector<FontFamily> families;
        std::map<std::pair<std::string, int>, FaceCoverage> coverage;  // by file and face number
    };
    std::shared_ptr<State> state_;  // shared with the (detached) scanning thread
};

// Sorted, de-duplicated family list for a set of faces (what FontIndex::families holds).
std::vector<FontFamily> list_families(const std::vector<FontFace>& faces);

// ---------------------------------------------------------------------------
// [fonts] config values -> the faces to load
// ---------------------------------------------------------------------------

struct ResolvedFonts {
    FontFiles files;                  // what load_fonts should load
    std::vector<std::string> errors;  // `font "X" not found` (for the error bar)
    bool waiting = false;             // a family name was left unresolved: the index isn't ready yet
};

// Turn the [fonts] values (paths or family names, spec 5 / 8a) into faces.
// `index` = the installed faces, or nullptr while the index is still being
// built (names then resolve to the built-in faces for now and `waiting` is set;
// no "not found" errors are reported until the index exists). `expand` turns a
// path value into the real path (~ and %VAR%).
//
// Rules:
// - prose by name: its family's regular pick; bold / italic / bold-italic come
//   from the family's automatic picks, and a style the family lacks uses the
//   family's regular face.
// - prose by path: that file's first face; the styles left "" use that face too.
// - prose "": built-in, and the styles left "" use the built-in styles.
// - prose_bold / prose_italic / prose_bold_italic set explicitly override the
//   automatic pick: a path = that file; a name = that family's pick for this
//   style, or its regular face if it has none.
// - mono by name: the family's regular face; by path: that file.
// - an unknown name: an error, and the slot behaves as if the value were "".
ResolvedFonts resolve_fonts(const Config::Fonts& values, const std::vector<FontFace>* index,
                            const std::function<std::string(const std::string&)>& expand);
