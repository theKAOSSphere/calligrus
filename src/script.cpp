#include "script.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "editor.h"
#include "pages.h"

namespace script {

static const char* const kNamedKeys[] = {"esc", "enter", "tab", "space", "up", "down", "left", "right", "pageup",
                                         "pagedown", "home", "end", "f5", "f11", "backspace"};

static std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e--;
    return s.substr(b, e - b);
}

// Decode one UTF-8 character (the whole string must be exactly one).
static std::optional<char32_t> single_char(const std::string& s) {
    if (s.empty()) return std::nullopt;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    const size_t len = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : (c0 >> 3) == 30 ? 4 : 0;
    if (len == 0 || s.size() != len) return std::nullopt;
    char32_t cp = len == 1 ? c0 : c0 & (0xFF >> (len + 1));
    for (size_t i = 1; i < len; i++) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    return cp;
}

bool export_outcome(const std::string& output, size_t& slides, std::string& error) {
    std::istringstream in(output);
    std::string line, err_line, last;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        line.erase(0, line.find_first_not_of(' '));
        if (line.rfind("exported ", 0) == 0) {
            const std::string n = line.substr(9, line.find(' ', 9) - 9);
            if (!n.empty() && n.find_first_not_of("0123456789") == std::string::npos) {
                slides = size_t(std::stoull(n));
                return true;
            }
        }
        if (err_line.empty() && line.rfind("export: ", 0) == 0) err_line = line.substr(8);
        if (!line.empty()) last = line;
    }
    error = !err_line.empty() ? err_line : !last.empty() ? last : "no answer from the export";
    return false;
}

std::optional<KeyToken> parse_key_token(const std::string& token) {
    KeyToken k;
    std::string rest = token;
    // Peel "ctrl+", "shift+", "alt+" prefixes (a lone "+" is the plus character).
    for (;;) {
        const size_t plus = rest.find('+');
        if (plus == std::string::npos || plus == 0 || plus + 1 >= rest.size()) break;
        const std::string mod = lower(rest.substr(0, plus));
        if (mod == "ctrl") k.ctrl = true;
        else if (mod == "shift") k.shift = true;
        else if (mod == "alt") k.alt = true;
        else return std::nullopt;
        rest = rest.substr(plus + 1);
    }
    const std::string name = lower(rest);
    if (name == "plus") {  // "ctrl+plus", "ctrl+minus"
        k.ch = U'+';
        return k;
    }
    if (name == "minus") {
        k.ch = U'-';
        return k;
    }
    for (const char* n : kNamedKeys)
        if (name == n) {
            k.named = name;
            return k;
        }
    auto ch = single_char(rest);
    if (!ch) return std::nullopt;
    k.ch = *ch;
    return k;
}

// A modifier word (`ctrl`, `shift`, `alt`) after the coordinates of click / move / drag.
static bool modifier_word(const std::string& w, Command& c) {
    if (w == "ctrl" || w == "control") c.mod_ctrl = true;
    else if (w == "shift") c.mod_shift = true;
    else if (w == "alt") c.mod_alt = true;
    else return false;
    return true;
}

static bool parse_number(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return !s.empty() && end && *end == 0;
}

Parsed parse(const std::string& text_in) {
    Parsed out;
    std::string text = text_in;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // UTF-8 BOM
    std::istringstream in(text);
    std::string raw;
    int line_no = 0;
    auto fail = [&](const std::string& msg) {
        out.error = "line " + std::to_string(line_no) + ": " + msg;
        out.commands.clear();
    };
    while (std::getline(in, raw)) {
        line_no++;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const size_t sp = line.find_first_of(" \t");
        const std::string cmd = lower(sp == std::string::npos ? line : line.substr(0, sp));
        const std::string rest = sp == std::string::npos ? "" : trim(line.substr(sp));
        std::vector<std::string> args;  // whitespace-separated words of `rest`
        {
            std::istringstream ws(rest);
            std::string w;
            while (ws >> w) args.push_back(w);
        }
        Command c;
        c.line = line_no;
        // The first `n` arguments as numbers into a, b, c, d.
        auto nums = [&](size_t n) -> bool {
            double* slots[] = {&c.a, &c.b, &c.c, &c.d};
            if (args.size() < n) return false;
            for (size_t i = 0; i < n; i++)
                if (!parse_number(args[i], *slots[i])) return false;
            return true;
        };
        if (cmd == "size") {
            c.kind = Kind::Size;
            if (args.size() != 2 || !nums(2) || c.a < 1 || c.b < 1 || c.a > 16384 || c.b > 16384) return fail("usage: size W H"), out;
        } else if (cmd == "scale") {
            c.kind = Kind::Scale;
            if (args.size() != 1 || !nums(1) || c.a <= 0 || c.a > 8) return fail("usage: scale S"), out;
        } else if (cmd == "open") {
            c.kind = Kind::Open;
            c.text = rest;
            if (rest.empty()) return fail("usage: open PATH"), out;
        } else if (cmd == "keys") {
            c.kind = Kind::Keys;
            if (args.empty()) return fail("usage: keys TOKENS"), out;
            for (const std::string& a : args) {
                auto k = parse_key_token(a);
                if (!k) return fail("bad key token '" + a + "'"), out;
                c.keys.push_back(*k);
            }
        } else if (cmd == "type") {
            c.kind = Kind::Type;
            // The rest of the line, exactly (one separating space dropped, trailing CR removed).
            c.text = sp == std::string::npos ? "" : line.substr(sp + 1);
            if (c.text.empty()) return fail("usage: type TEXT"), out;
        } else if (cmd == "click") {
            c.kind = Kind::Click;
            if (args.size() < 2 || !nums(2)) return fail("usage: click X Y [left|right|middle|x1|x2] [ctrl] [shift] [alt]"), out;
            // After the coordinates: a button word and/or modifier words, in any order.
            for (size_t i = 2; i < args.size(); i++) {
                const std::string b = lower(args[i]);
                if (b == "left") c.button = 0;
                else if (b == "right") c.button = 1;
                else if (b == "middle") c.button = 2;
                else if (b == "x1") c.button = 3;
                else if (b == "x2") c.button = 4;
                else if (!modifier_word(b, c)) return fail("unknown mouse button or modifier '" + args[i] + "'"), out;
            }
        } else if (cmd == "move") {
            c.kind = Kind::Move;
            if (args.size() < 2 || !nums(2)) return fail("usage: move X Y [ctrl] [shift] [alt]"), out;
            for (size_t i = 2; i < args.size(); i++)
                if (!modifier_word(lower(args[i]), c)) return fail("unknown modifier '" + args[i] + "'"), out;
        } else if (cmd == "drag") {
            c.kind = Kind::Drag;
            if (args.size() < 4 || !nums(4)) return fail("usage: drag X1 Y1 X2 Y2 [ctrl] [shift] [alt]"), out;
            for (size_t i = 4; i < args.size(); i++)
                if (!modifier_word(lower(args[i]), c)) return fail("unknown modifier '" + args[i] + "'"), out;
        } else if (cmd == "wheel") {
            c.kind = Kind::Wheel;
            if (args.size() == 1 && nums(1)) {
            } else if (args.size() == 3 && nums(3)) {
                c.has_pos = true;
            } else {
                return fail("usage: wheel DY [X Y]"), out;
            }
        } else if (cmd == "settle") {
            c.kind = Kind::Settle;
            if (!args.empty()) return fail("settle takes no arguments"), out;
        } else if (cmd == "frames") {
            c.kind = Kind::Frames;
            if (args.size() != 1 || !nums(1) || c.a < 0) return fail("usage: frames N"), out;
        } else if (cmd == "slide") {
            c.kind = Kind::Slide;  // slides mode: show slide N (1-based), all its steps
            if (args.size() != 1 || !nums(1) || c.a < 1 || c.a != double(int(c.a))) return fail("usage: slide N (from 1)"), out;
        } else if (cmd == "export") {
            c.kind = Kind::Export;  // the "PDF" button without its dialog: export to FILE and wait
            c.text = rest;
            if (rest.empty()) return fail("usage: export FILE.pdf"), out;
        } else if (cmd == "export-pages") {
            c.kind = Kind::ExportPages;  // "Pages..." without its dialog (spec 29)
            c.text = rest;
            if (rest.empty()) return fail("usage: export-pages FILE.pdf"), out;
        } else if (cmd == "shot") {
            c.kind = Kind::Shot;
            c.text = rest;
            if (rest.empty()) return fail("usage: shot FILE.png"), out;
        } else if (cmd == "echo") {
            c.kind = Kind::Echo;
            c.text = sp == std::string::npos ? "" : line.substr(sp + 1);
        } else if (cmd == "expect-title") {
            c.kind = Kind::ExpectTitle;
            c.text = sp == std::string::npos ? "" : line.substr(sp + 1);
        } else if (cmd == "expect-tabs") {
            c.kind = Kind::ExpectTabs;  // expect-tabs N ACTIVE (spec 34)
            if (args.size() != 2 || !nums(2) || c.a < 1 || c.b < 1 || c.b > c.a || c.a != double(int(c.a)) || c.b != double(int(c.b)))
                return fail("usage: expect-tabs N ACTIVE (1-based, ACTIVE <= N)"), out;
        } else if (cmd == "expect-find") {
            c.kind = Kind::ExpectFind;  // expect-find closed | expect-find field|doc MATCHES CURRENT (spec 7)
            const std::string st = args.empty() ? "" : lower(args[0]);
            c.text = st;
            if (st == "closed" && args.size() == 1) {
            } else if ((st == "field" || st == "doc") && args.size() == 3) {
                double* slots[] = {&c.a, &c.b};
                for (size_t i = 0; i < 2; i++)
                    if (!parse_number(args[i + 1], *slots[i]) || *slots[i] < 0) return fail("usage: expect-find closed | expect-find field|doc MATCHES CURRENT"), out;
            } else {
                return fail("usage: expect-find closed | expect-find field|doc MATCHES CURRENT"), out;
            }
        } else if (cmd == "expect-editor") {
            c.kind = Kind::ExpectEditor;
            if (rest.empty()) return fail("`expect-editor` needs a JSON message, or `none`"), out;
            if (rest == "none") {
                c.text.clear();
            } else {
                c.text = rest;
                if (!editor::valid_json_object(rest)) return fail("bad expect-editor JSON"), out;
            }
        } else if (cmd == "editor") {
            c.kind = Kind::Editor;
            c.text = rest;  // one editor protocol message (JSON, spec 15)
            if (rest.empty()) return fail("usage: editor JSON"), out;
        } else if (cmd == "sync-dump") {
            c.kind = Kind::SyncDump;
            c.text = rest;  // output TSV file (spec 16)
            if (rest.empty()) return fail("usage: sync-dump FILE"), out;
        } else if (cmd == "quit") {
            c.kind = Kind::Quit;
        } else {
            return fail("unknown command '" + cmd + "'"), out;
        }
        out.commands.push_back(std::move(c));
    }
    return out;
}

std::optional<Export> export_args(const std::vector<std::string>& args, std::string& error) {
    Export e;
    bool found = false, scale_given = false;
    auto number = [](const std::string& s, double& v) {
        char* end = nullptr;
        v = std::strtod(s.c_str(), &end);
        return !s.empty() && end == s.c_str() + s.size();
    };
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        const bool more = i + 1 < args.size();
        if (a == "--export") {
            found = true;
            if (!more) return error = "--export needs an output file (.pdf) or folder", std::nullopt;
            e.out = args[++i];
        } else if (a == "--size") {
            if (!more) return error = "--size needs WIDTHxHEIGHT", std::nullopt;
            const std::string v = args[++i];
            const size_t x = v.find_first_of("xX");
            double w = 0, h = 0;
            if (x == std::string::npos) return error = "bad --size " + v + ": expected WIDTHxHEIGHT", std::nullopt;
            if (!number(v.substr(0, x), w) || w != double(int(w)) || w < 100 || w > 8000) return error = "bad --size width " + v.substr(0, x), std::nullopt;
            if (!number(v.substr(x + 1), h) || h != double(int(h)) || h < 100 || h > 8000) return error = "bad --size height " + v.substr(x + 1), std::nullopt;
            e.width = int(w), e.height = int(h);
        } else if (a == "--scale") {
            if (!more) return error = "--scale needs a number", std::nullopt;
            double s = 0;
            if (!number(args[++i], s) || s <= 0 || s > 4) return error = "bad --scale " + args[i], std::nullopt;
            e.scale = float(s);
            scale_given = true;
        } else if (a == "--pages") {
            e.pages = true;
        } else if (a == "--raster") {
            e.raster = true;
        } else if (a == "--paper") {
            float w = 0, h = 0;
            if (!more) return error = "--paper needs a4 or letter", std::nullopt;
            if (!pages::paper_size(args[++i], w, h)) return error = "bad --paper " + args[i] + ": expected a4 or letter", std::nullopt;
            e.paper = lower(args[i]);
        } else if (a == "--theme") {
            if (!more) return error = "--theme needs a theme name", std::nullopt;
            e.theme = args[++i];
        } else if (a == "--base-dir") {
            if (!more) return error = "--base-dir needs a folder", std::nullopt;
            e.base_dir = args[++i];
        } else if (a == "--body-size") {
            if (!more) return error = "--body-size needs a number of points", std::nullopt;
            double pt = 0;
            if (!number(args[++i], pt) || pt < pages::kBodyPtMin || pt > pages::kBodyPtMax) return error = "bad --body-size " + args[i] + ": expected 8 to 64", std::nullopt;
            e.body_size = float(pt);
        } else if (a.starts_with("--")) {
        } else if (e.input.empty()) {
            e.input = a;
        }
    }
    if (!found) return std::nullopt;
    if (e.pages && !scale_given) e.scale = 2.0f;  // sharper than slides: a page may be printed
    if (e.pages && !lower(e.out).ends_with(".pdf")) return error = "--pages needs an output file ending in .pdf", std::nullopt;
    if (e.input.empty()) return error = "--export needs the file to export", std::nullopt;
    return e;
}

std::filesystem::path editor_base_dir(const std::filesystem::path& path) {
    std::filesystem::path p = path.parent_path();
    return p.empty() ? std::filesystem::path(".") : p;
}

std::string editor_pdf_name(const std::filesystem::path& path) {
    const std::u8string stem = path.stem().u8string();
    const std::string s(stem.begin(), stem.end());
    return (s.empty() ? std::string("untitled") : s) + ".pdf";
}

std::filesystem::path editor_temp_file(bool org) {
    static unsigned counter = 0;
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / ("calligrus-export-" + std::to_string(tick) + "-" + std::to_string(counter++) + (org ? ".org" : ".md"));
}

}  // namespace script
