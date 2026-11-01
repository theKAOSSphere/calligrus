// Headless script mode (spec section 14): the script file format and its
// parser. No window or ImGui in here, so it can be unit-tested (script_test.cpp);
// main.cpp runs the commands.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace script {

// One key press from a `keys` token: modifiers plus a named key ("esc", "f5"...)
// or a single character ('j', 'G', '/').
struct KeyToken {
    bool ctrl = false, shift = false, alt = false;
    std::string named;  // non-empty: a named key
    char32_t ch = 0;    // otherwise: this character ('G' implies shift)
};

enum class Kind { Size, Scale, Open, Keys, Type, Click, Move, Drag, Wheel, Settle, Frames, Slide, Export, ExportPages, PagesBegin, PageShot, Shot, Echo, ExpectTitle, ExpectTabs, ExpectFind, ExpectEditor, Editor, SyncDump, Quit };

struct Command {
    Kind kind = Kind::Quit;
    int line = 0;                       // 1-based, for error messages
    std::string text;                   // path / file name / text, depending on the command
    std::vector<KeyToken> keys;         // Keys
    double a = 0, b = 0, c = 0, d = 0;  // numbers: Size W H; Click X Y; Drag X1 Y1 X2 Y2; Wheel DY [X Y]; Frames N; Slide N
    int button = 0;                     // Click: 0 left, 1 right, 2 middle, 3 x1, 4 x2
    bool has_pos = false;               // Wheel: X Y given
    bool mod_ctrl = false, mod_shift = false, mod_alt = false;  // Click / Move / Drag: modifier words (spec 23)
    // ExpectTabs: `a` = how many tabs must be open, `b` = which one (1-based) is active (spec 34).
    // ExpectFind: `text` = closed | field | doc (the find bar is closed / open with the keys in its field / open with the keys on
    // the document), `a` = matches, `b` = the current one (1-based, 0 = none) (spec 7).
    // ExpectEditor: `text` is the JSON, or empty for `expect-editor none`.
};

struct Parsed {
    std::vector<Command> commands;
    std::string error;  // empty = ok; otherwise "line N: message"
};

Parsed parse(const std::string& text);

// Slides export (spec 26): `--export OUT FILE [--size WxH] [--scale S]`. OUT ending in `.pdf` is a
// PDF, anything else a folder for slide-001.png...
struct Export {
    std::string input, out;
    int width = 1280, height = 720;  // the window the slides are drawn in (points)...
    // `--pages` (spec 29): export the document as real pages instead of slides (width and height are
    // then unused, scale defaults to 2), on this paper ("a4" / "letter"), in this theme ("" = the
    // config's light_theme).
    bool pages = false;
    std::string paper = "a4", theme;
    float body_size = 0;             // `--body-size PT` (8..64 real points; 0 = the config's body_size x 0.75): the pages' text size
    std::string base_dir;            // `--base-dir DIR` ("" = the folder of FILE): where relative pictures and links resolve from
    float scale = 1.5f;              // ...and its pixels per point
    bool raster = false;             // `--raster` (spec 33): the old PDF, one picture per page (default: real text over a picture without text)
};
// The export command line in `args` (without the program name): nullopt if there is no `--export`;
// `error` is set (and nullopt returned) if it is incomplete or a value is bad.
std::optional<Export> export_args(const std::vector<std::string>& args, std::string& error);
std::optional<KeyToken> parse_key_token(const std::string& token);

// Exporting what an editor shows (spec 27): its text goes to a temporary file, the child export reads
// that, and `--base-dir` points at the real document's folder. These are the names (as script.rs).
// The folder relative pictures and links resolve from: the folder of the editor's path, else ".".
std::filesystem::path editor_base_dir(const std::filesystem::path& path);
// The save dialog's suggested name: `<stem>.pdf`, `untitled.pdf` for a buffer without a path.
std::string editor_pdf_name(const std::filesystem::path& path);
// A fresh temp-folder file name for the text: `.org` for Org, `.md` for Markdown.
std::filesystem::path editor_temp_file(bool org);

// What an export child process (`--export`, started by the app's "PDF" button) printed: true and
// the number of slides it wrote, or false and its error message (spec 27; as script.rs).
bool export_outcome(const std::string& output, size_t& slides, std::string& error);

}  // namespace script
