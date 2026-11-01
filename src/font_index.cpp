#include "font_index.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <thread>

#include "trace.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Names and paths
// ---------------------------------------------------------------------------

static char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

static std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

bool same_family(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

bool is_font_family_name(std::string_view value) {
    if (value.empty()) return false;
    if (value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos) return false;
    const std::string l = lower(value);
    for (const char* ext : {".ttf", ".otf", ".ttc"})
        if (l.ends_with(ext)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Picking a family's faces
// ---------------------------------------------------------------------------

static bool contains_ci(std::string_view hay, std::string_view needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

bool is_italic_face(const FontFace& f) {
    return f.italic || contains_ci(f.subfamily, "italic") || contains_ci(f.subfamily, "oblique");
}

FacePicks pick_faces(const std::vector<FontFace>& faces, std::string_view family) {
    // The best face of the family with the given slant for a target weight
    // (closest weight; ties: lower weight, then file path in byte order, then
    // face index as a number). std::string compares bytes as unsigned, like memcmp.
    auto best = [&](bool italic, int target, int min_weight) -> std::optional<FontFace> {
        const FontFace* pick = nullptr;
        for (const FontFace& f : faces) {
            if (is_italic_face(f) != italic || f.weight < min_weight || !same_family(f.family, family)) continue;
            if (!pick) {
                pick = &f;
                continue;
            }
            // Width closest to normal (5) first; an out-of-range width counts as 5.
            auto width_dist = [](int w) { return w >= 1 && w <= 9 ? std::abs(w - 5) : 0; };
            const int wd = width_dist(f.width), pwd = width_dist(pick->width);
            const int d = std::abs(f.weight - target), pd = std::abs(pick->weight - target);
            const bool better = wd != pwd                ? wd < pwd
                                : d != pd                ? d < pd
                                : f.weight != pick->weight ? f.weight < pick->weight
                                : f.file != pick->file     ? f.file < pick->file
                                                           : f.face < pick->face;
            if (better) pick = &f;
        }
        return pick ? std::optional<FontFace>(*pick) : std::nullopt;
    };
    FacePicks p;
    p.regular = best(false, 400, 0);
    p.bold = best(false, 700, 600);
    p.italic = best(true, 400, 0);
    p.bold_italic = best(true, 700, 600);
    if (!p.regular) p.regular = p.italic;  // an italic-only family: its italic closest to 400 is "regular"
    return p;
}

std::vector<FontFamily> list_families(const std::vector<FontFace>& faces) {
    // Group by lower-case family name, then describe each by its regular face.
    std::map<std::string, std::vector<FontFace>> groups;
    for (const FontFace& f : faces)
        if (!f.family.empty()) groups[lower(f.family)].push_back(f);
    std::vector<FontFamily> out;
    for (const auto& [key, group] : groups) {
        // Spelled as on the regular pick; monospace if ANY face of the family is.
        const FacePicks p = pick_faces(group, group.front().family);
        const bool mono = std::any_of(group.begin(), group.end(), [](const FontFace& f) { return f.mono; });
        out.push_back({p.regular ? p.regular->family : group.front().family, mono});
    }
    return out;  // std::map kept them sorted by the lower-case name (one family per lower-case name)
}

// ---------------------------------------------------------------------------
// [fonts] values -> faces
// ---------------------------------------------------------------------------

ResolvedFonts resolve_fonts(const Config::Fonts& v, const std::vector<FontFace>* index,
                            const std::function<std::string(const std::string&)>& expand) {
    ResolvedFonts r;
    auto source = [](const FontFace& f) { return FontSource{f.file, f.face}; };

    // A family's picks; nullopt if the name is unknown (error) or the index isn't ready (waiting).
    auto family = [&](const std::string& name) -> std::optional<FacePicks> {
        if (!index) {
            r.waiting = true;
            return std::nullopt;
        }
        FacePicks p = pick_faces(*index, name);
        if (!p.regular) {
            const std::string msg = "font \"" + name + "\" not found";
            if (std::find(r.errors.begin(), r.errors.end(), msg) == r.errors.end()) r.errors.push_back(msg);
            return std::nullopt;
        }
        return p;
    };
    // (pick_faces always sets `regular` for a known family.)
    auto regular_of = [](const FacePicks& p) { return *p.regular; };

    // --- prose (regular) ---
    std::optional<FacePicks> prose_family;  // set when prose is a known family name
    if (is_font_family_name(v.prose)) {
        prose_family = family(v.prose);
        if (prose_family) r.files.prose = source(regular_of(*prose_family));
    } else if (!v.prose.empty()) {
        r.files.prose = FontSource{expand(v.prose), 0};
    }

    // --- bold / italic / bold-italic ---
    using Pick = std::optional<FontFace> FacePicks::*;
    auto style = [&](const std::string& value, Pick pick) -> FontSource {
        if (is_font_family_name(value)) {
            if (auto p = family(value)) return source((*p).*pick ? *((*p).*pick) : regular_of(*p));
        } else if (!value.empty()) {
            return FontSource{expand(value), 0};
        }
        // Not set (or an unknown name): the automatic pick of the prose family,
        // else the prose face itself (built-in prose -> built-in style: "").
        if (prose_family && (*prose_family).*pick) return source(*((*prose_family).*pick));
        return r.files.prose;
    };
    r.files.prose_bold = style(v.prose_bold, &FacePicks::bold);
    r.files.prose_italic = style(v.prose_italic, &FacePicks::italic);
    r.files.prose_bold_italic = style(v.prose_bold_italic, &FacePicks::bold_italic);

    // --- mono ---
    if (is_font_family_name(v.mono)) {
        if (auto p = family(v.mono)) r.files.mono = source(regular_of(*p));
    } else if (!v.mono.empty()) {
        r.files.mono = FontSource{expand(v.mono), 0};
    }
    return r;
}

// ---------------------------------------------------------------------------
// Reading the sfnt tables
// ---------------------------------------------------------------------------
//
// An OpenType file starts with a table directory: a 12-byte header (the number
// of tables at offset 4) and a 16-byte record per table (tag, checksum,
// offset, length). A .ttc starts with "ttcf", the number of faces at offset 8
// and the offset of each face's table directory from offset 12.
//
// Everything goes through a `fetch(offset, length)` function so a file on disk
// is read piece by piece (only the few tables we need, not the whole file:
// some CJK fonts are 20+ MB).

namespace {

using Fetch = std::function<std::string(uint64_t offset, size_t length)>;

uint32_t be16(std::string_view b, size_t at) {
    return at + 2 <= b.size() ? (uint32_t(uint8_t(b[at])) << 8) | uint8_t(b[at + 1]) : 0;
}
uint32_t be32(std::string_view b, size_t at) { return (be16(b, at) << 16) | be16(b, at + 2); }

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

std::string utf16be_to_utf8(std::string_view b) {
    std::string out;
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        uint32_t c = be16(b, i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < b.size()) {  // surrogate pair
            const uint32_t lo = be16(b, i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        append_utf8(out, c);
    }
    return out;
}

// Mac Roman names (platform 1): ASCII is all that matters for family names;
// other bytes are taken as Latin-1 (close enough for display).
std::string latin1_to_utf8(std::string_view b) {
    std::string out;
    for (char c : b) append_utf8(out, uint8_t(c));
    return out;
}

// The `name` table string for `id`, preferring Windows / Unicode English.
std::string read_name(std::string_view table, uint32_t id) {
    const uint32_t count = be16(table, 2), strings = be16(table, 4);
    int best_score = 0;
    std::string best;
    for (uint32_t i = 0; i < count; i++) {
        const size_t rec = 6 + size_t(i) * 12;
        if (rec + 12 > table.size()) break;
        if (be16(table, rec + 6) != id) continue;
        const uint32_t platform = be16(table, rec), encoding = be16(table, rec + 2), language = be16(table, rec + 4);
        int score = 0;
        if (platform == 3 && (encoding == 0 || encoding == 1 || encoding == 10)) score = language == 0x409 ? 5 : 3;
        else if (platform == 0) score = 2;
        else if (platform == 1 && encoding == 0) score = language == 0 ? 1 : 0;
        if (score <= best_score) continue;
        const size_t len = be16(table, rec + 8), off = size_t(strings) + be16(table, rec + 10);
        if (off + len > table.size()) continue;
        const std::string_view raw = table.substr(off, len);
        std::string s = platform == 1 ? latin1_to_utf8(raw) : utf16be_to_utf8(raw);
        // Trim stray spaces / NULs some fonts pad names with.
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
        if (s.empty()) continue;
        best_score = score;
        best = std::move(s);
    }
    return best;
}

// One face whose table directory starts at `dir`.
std::optional<FontFace> read_face(const Fetch& fetch, uint64_t dir, const std::string& file, int face_index) {
    const std::string header = fetch(dir, 12);
    const uint32_t n_tables = be16(header, 4);
    if (header.size() < 12 || n_tables == 0 || n_tables > 512) return std::nullopt;
    const std::string records = fetch(dir + 12, size_t(n_tables) * 16);
    auto table = [&](const char* tag, size_t max_len) -> std::string {
        for (uint32_t i = 0; i < n_tables; i++) {
            const size_t rec = size_t(i) * 16;
            if (rec + 16 > records.size()) break;
            if (records.compare(rec, 4, tag) == 0) {
                const uint32_t len = be32(records, rec + 12);
                return fetch(be32(records, rec + 8), std::min<size_t>(len, max_len));
            }
        }
        return {};
    };
    const std::string name = table("name", 1 << 20), os2 = table("OS/2", 100), post = table("post", 32);
    if (name.size() < 6) return std::nullopt;

    FontFace f;
    f.file = file;
    f.face = face_index;
    f.family = read_name(name, 16);
    if (f.family.empty()) f.family = read_name(name, 1);
    f.subfamily = read_name(name, 17);
    if (f.subfamily.empty()) f.subfamily = read_name(name, 2);
    if (f.family.empty()) return std::nullopt;

    if (os2.size() >= 64) {
        int w = int(be16(os2, 4));                // usWeightClass
        if (w >= 1 && w <= 9) w *= 100;           // some old fonts use the 1..9 scale
        if (w >= 1 && w <= 1000) f.weight = w;
        const int width = int(be16(os2, 6));      // usWidthClass: 1 ultra-condensed .. 5 normal .. 9 ultra-expanded
        if (width >= 1 && width <= 9) f.width = width;
        f.italic = (be16(os2, 62) & 1) != 0;      // fsSelection bit 0: ITALIC
        f.mono = uint8_t(os2[35]) == 9;           // panose bProportion == 9: monospaced
    }
    if (contains_ci(f.subfamily, "italic") || contains_ci(f.subfamily, "oblique")) f.italic = true;
    if (post.size() >= 16 && be32(post, 12) != 0) f.mono = true;  // post.isFixedPitch
    return f;
}

std::vector<FontFace> parse_faces(const Fetch& fetch, const std::string& file) {
    std::vector<FontFace> out;
    const std::string head = fetch(0, 12);
    if (head.size() < 12) return out;
    if (head.compare(0, 4, "ttcf") == 0) {
        const uint32_t n = be32(head, 8);
        if (n == 0 || n > 1024) return out;
        const std::string offsets = fetch(12, size_t(n) * 4);
        for (uint32_t i = 0; i < n && size_t(i) * 4 + 4 <= offsets.size(); i++)
            if (auto f = read_face(fetch, be32(offsets, size_t(i) * 4), file, int(i))) out.push_back(*f);
        return out;
    }
    // TrueType outlines (0x00010000 or "true") or CFF ("OTTO").
    const uint32_t tag = be32(head, 0);
    if (tag != 0x00010000 && head.compare(0, 4, "OTTO") != 0 && head.compare(0, 4, "true") != 0) return out;
    if (auto f = read_face(fetch, 0, file, 0)) out.push_back(*f);
    return out;
}

std::string to_utf8(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

}  // namespace

std::vector<FontFace> parse_font_faces(std::string_view data, const std::string& file) {
    return parse_faces(
        [&](uint64_t off, size_t len) -> std::string {
            if (off >= data.size()) return {};
            return std::string(data.substr(size_t(off), len));
        },
        file);
}

std::vector<FontFace> read_font_faces(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return parse_faces(
        [&](uint64_t off, size_t len) -> std::string {
            std::string buf(len, '\0');
            in.clear();
            in.seekg(std::streamoff(off));
            in.read(buf.data(), std::streamsize(len));
            buf.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
            return buf;
        },
        to_utf8(file));
}

bool read_font_cmap(const fs::path& file, int face, std::string& cmap, bool& colour) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    auto fetch = [&](uint64_t off, size_t len) -> std::string {
        std::string buf(len, '\0');
        in.clear();
        in.seekg(std::streamoff(off));
        in.read(buf.data(), std::streamsize(len));
        buf.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
        return buf;
    };
    uint64_t dir = 0;  // where this face's table directory starts
    const std::string head = fetch(0, 12);
    if (head.size() < 12) return false;
    if (head.compare(0, 4, "ttcf") == 0) {
        if (face < 0 || uint32_t(face) >= be32(head, 8)) return false;
        dir = be32(fetch(12 + uint64_t(face) * 4, 4), 0);
    } else if (face != 0) {
        return false;
    }
    const std::string header = fetch(dir, 12);
    const uint32_t n_tables = be16(header, 4);
    if (header.size() < 12 || n_tables == 0 || n_tables > 512) return false;
    const std::string records = fetch(dir + 12, size_t(n_tables) * 16);
    colour = false;
    bool found = false;
    for (uint32_t i = 0; i < n_tables && size_t(i) * 16 + 16 <= records.size(); i++) {
        const size_t rec = size_t(i) * 16;
        for (const char* tag : {"COLR", "CBDT", "sbix", "SVG "})
            if (records.compare(rec, 4, tag) == 0) colour = true;
        if (records.compare(rec, 4, "cmap") == 0) {
            cmap = fetch(be32(records, rec + 8), std::min<size_t>(be32(records, rec + 12), size_t(1) << 22));
            found = !cmap.empty();
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// Character coverage
// ---------------------------------------------------------------------------

bool FaceCoverage::has(char32_t c) const {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), uint32_t(c), [](uint32_t v, const std::pair<uint32_t, uint32_t>& r) { return v < r.first; });
    return it != ranges.begin() && c <= std::prev(it)->second;
}

std::vector<std::pair<uint32_t, uint32_t>> cmap_ranges(std::string_view cmap) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    auto add = [&](uint32_t lo, uint32_t hi) {
        if (lo <= hi) out.push_back({lo, hi});
    };
    const uint32_t count = be16(cmap, 2);
    for (uint32_t i = 0; i < count; i++) {
        const size_t rec = 4 + size_t(i) * 8;
        const uint32_t platform = be16(cmap, rec), encoding = be16(cmap, rec + 2);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        const size_t offset = be32(cmap, rec + 4);
        if (!unicode || offset >= cmap.size()) continue;
        const std::string_view t = cmap.substr(offset);
        switch (be16(t, 0)) {
            case 4: {  // segments of consecutive codes (the BMP): a code per code, as a segment may skip some
                const size_t seg_count = be16(t, 6) / 2;
                const size_t ends = 14, starts = 16 + seg_count * 2, deltas = starts + seg_count * 2, offsets = deltas + seg_count * 2;
                for (size_t k = 0; k < seg_count; k++) {
                    const uint32_t end = be16(t, ends + k * 2), start = be16(t, starts + k * 2), delta = be16(t, deltas + k * 2);
                    const uint32_t range_offset = be16(t, offsets + k * 2);
                    if (start > end || (start == 0xFFFF && end == 0xFFFF)) continue;
                    uint32_t run = 0;
                    bool in_run = false;
                    for (uint32_t c = start; c <= end; c++) {
                        bool has;
                        if (range_offset == 0) has = ((c + delta) & 0xFFFF) != 0;
                        else has = be16(t, offsets + k * 2 + range_offset + size_t(c - start) * 2) != 0;
                        if (has && !in_run) run = c, in_run = true;
                        if (!has && in_run) add(run, c - 1), in_run = false;
                    }
                    if (in_run) add(run, end);
                }
                break;
            }
            case 6: {  // one run of consecutive codes
                const uint32_t first = be16(t, 6), n = be16(t, 8);
                uint32_t run = 0;
                bool in_run = false;
                for (uint32_t c = first; c < first + n; c++) {
                    const bool has = be16(t, 10 + size_t(c - first) * 2) != 0;
                    if (has && !in_run) run = c, in_run = true;
                    if (!has && in_run) add(run, c - 1), in_run = false;
                }
                if (in_run) add(run, first + n - 1);
                break;
            }
            case 12:
            case 13: {  // groups of consecutive codes (all of Unicode)
                const uint32_t groups = std::min<uint32_t>(be32(t, 12), uint32_t(t.size() / 12));
                for (uint32_t k = 0; k < groups; k++) {
                    const size_t g = 16 + size_t(k) * 12;
                    const uint32_t start = be32(t, g), end = be32(t, g + 4), glyph = be32(t, g + 8);
                    if (be16(t, 0) == 12) add(glyph == 0 ? start + 1 : start, end);  // (glyph 0 is "no glyph": the first code of a run starting there)
                    else if (glyph != 0) add(start, end);
                }
                break;
            }
            default: break;
        }
    }
    std::sort(out.begin(), out.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto& r : out) {
        if (!merged.empty() && r.first <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    return merged;
}

// ---------------------------------------------------------------------------
// The index
// ---------------------------------------------------------------------------

static fs::path env_path(const char* name) {
#pragma warning(suppress : 4996)  // MSVC: getenv is fine here (read once, on start-up)
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
}

std::vector<fs::path> FontIndex::system_font_dirs() {
    std::vector<fs::path> dirs;
#ifdef _WIN32
    fs::path windir = env_path("WINDIR");
    if (windir.empty()) windir = env_path("SystemRoot");
    if (windir.empty()) windir = "C:\\Windows";
    dirs.push_back(windir / "Fonts");
    if (fs::path local = env_path("LOCALAPPDATA"); !local.empty()) dirs.push_back(local / "Microsoft" / "Windows" / "Fonts");
#elif defined(__ANDROID__)
    // The system's own fonts (Roboto, the Noto fonts for Devanagari, Gujarati, CJK, ...), spec 35.
    for (const char* d : {"/system/fonts", "/product/fonts", "/system_ext/fonts"}) dirs.push_back(d);
#else
    // The folders fontconfig scans by default (fonts live in sub-folders: the scan below recurses).
    dirs.push_back("/usr/share/fonts");
    dirs.push_back("/usr/local/share/fonts");
    if (fs::path home = env_path("HOME"); !home.empty()) dirs.push_back(home / ".fonts");
    fs::path data_home = env_path("XDG_DATA_HOME");
    if (data_home.empty() && !env_path("HOME").empty()) data_home = env_path("HOME") / ".local" / "share";
    if (!data_home.empty()) dirs.push_back(data_home / "fonts");
    if (const char* xdg = std::getenv("XDG_DATA_DIRS")) {
        std::string_view all(xdg);
        for (size_t at = 0; at < all.size();) {
            size_t colon = all.find(':', at);
            if (colon == std::string_view::npos) colon = all.size();
            if (colon > at) {
                fs::path d = fs::path(std::string(all.substr(at, colon - at))) / "fonts";
                if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
            }
            at = colon + 1;
        }
    }
#endif
    return dirs;
}

fs::path FontIndex::default_cache_file() {
#ifdef _WIN32
    fs::path base = env_path("LOCALAPPDATA");
#else
    fs::path base = env_path("XDG_STATE_HOME");
    if (base.empty() && !env_path("HOME").empty()) base = env_path("HOME") / ".local" / "state";
#endif
    return base.empty() ? fs::path() : base / "calligrus" / "fontindex.cache";
}

namespace {

// ---- the saved index ------------------------------------------------------
//
// Text, one line per record, fields separated by tabs (UTF-8):
//   calligrus-fontindex 1
//   F <path> <size> <mtime> <number of faces>      a font file, then one `f` line per face:
//   f <family> <subfamily> <weight> <italic> <mono> <face no.> <width> <colour> <ranges: hex lo-hi,lo-hi or ->
// A file that holds no usable face is listed with 0 faces (it is not looked at again until it changes).

struct FileKey {
    std::string path;  // UTF-8
    uint64_t size = 0;
    int64_t mtime = 0;
    bool operator==(const FileKey&) const = default;
};

struct FileEntry {
    FileKey key;
    std::vector<FontFace> faces;
    std::vector<FaceCoverage> coverage;  // one per face
};

const char kCacheMagic[] = "calligrus-fontindex 1";

std::string clean(std::string s) {  // (names cannot hold the separators)
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::vector<std::string_view> split(std::string_view line, char sep) {
    std::vector<std::string_view> out;
    size_t at = 0;
    for (;;) {
        const size_t next = line.find(sep, at);
        if (next == std::string_view::npos) {
            out.push_back(line.substr(at));
            return out;
        }
        out.push_back(line.substr(at, next - at));
        at = next + 1;
    }
}

bool to_int(std::string_view s, int64_t& v, int base = 10) {
    if (s.empty()) return false;
    char* end = nullptr;
    const std::string z(s);
    v = std::strtoll(z.c_str(), &end, base);
    return end && *end == '\0';
}

std::map<std::string, FileEntry> load_cache(const fs::path& file) {
    std::map<std::string, FileEntry> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    std::string line;
    if (!std::getline(in, line) || line != kCacheMagic) return out;
    FileEntry* cur = nullptr;
    size_t want = 0;
    auto bad = [&] {
        out.clear();
        return out;
    };
    while (std::getline(in, line)) {
        const auto f = split(line, '\t');
        int64_t a = 0, b = 0, c = 0, d = 0, e = 0, g = 0;
        if (f.size() == 5 && f[0] == "F") {
            if (cur && cur->faces.size() != want) return bad();
            if (!to_int(f[2], a) || !to_int(f[3], b) || !to_int(f[4], c)) return bad();
            FileEntry entry;
            entry.key = {std::string(f[1]), uint64_t(a), b};
            cur = &out.emplace(entry.key.path, std::move(entry)).first->second;
            want = size_t(c);
        } else if (f.size() == 10 && f[0] == "f" && cur) {
            if (!to_int(f[3], a) || !to_int(f[4], b) || !to_int(f[5], c) || !to_int(f[6], d) || !to_int(f[7], e) || !to_int(f[8], g)) return bad();
            FontFace face;
            face.family = std::string(f[1]), face.subfamily = std::string(f[2]);
            face.weight = int(a), face.italic = b != 0, face.mono = c != 0, face.face = int(d), face.width = int(e);
            face.file = cur->key.path;
            FaceCoverage cov;
            cov.colour = g != 0;
            if (f[9] != "-")
                for (std::string_view r : split(f[9], ',')) {
                    const size_t dash = r.find('-');
                    int64_t lo = 0, hi = 0;
                    if (dash == std::string_view::npos || !to_int(r.substr(0, dash), lo, 16) || !to_int(r.substr(dash + 1), hi, 16)) return bad();
                    cov.ranges.push_back({uint32_t(lo), uint32_t(hi)});
                }
            cur->faces.push_back(std::move(face));
            cur->coverage.push_back(std::move(cov));
        } else {
            return bad();
        }
    }
    if (cur && cur->faces.size() != want) return bad();
    return out;
}

void save_cache(const fs::path& file, const std::vector<FileEntry>& entries) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = fs::path(file).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << kCacheMagic << '\n';
        for (const FileEntry& e : entries) {
            out << "F\t" << clean(e.key.path) << '\t' << e.key.size << '\t' << e.key.mtime << '\t' << e.faces.size() << '\n';
            for (size_t i = 0; i < e.faces.size(); i++) {
                const FontFace& f = e.faces[i];
                const FaceCoverage& cov = e.coverage[i];
                out << "f\t" << clean(f.family) << '\t' << clean(f.subfamily) << '\t' << f.weight << '\t' << int(f.italic) << '\t' << int(f.mono) << '\t' << f.face
                    << '\t' << f.width << '\t' << int(cov.colour) << '\t';
                if (cov.ranges.empty()) out << '-';
                char buf[32];
                for (size_t k = 0; k < cov.ranges.size(); k++) {
                    std::snprintf(buf, sizeof buf, "%s%x-%x", k ? "," : "", unsigned(cov.ranges[k].first), unsigned(cov.ranges[k].second));
                    out << buf;
                }
                out << '\n';
            }
        }
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp, ec);
}

// One font file read for real: its faces, and each face's coverage (the cmap table, as ranges).
FileEntry read_entry(const fs::path& file, const FileKey& key) {
    FileEntry e;
    e.key = key;
    e.faces = read_font_faces(file);
    for (const FontFace& f : e.faces) {
        FaceCoverage cov;
        std::string cmap;
        bool colour = false;
        if (read_font_cmap(file, f.face, cmap, colour)) cov.ranges = cmap_ranges(cmap);
        cov.colour = colour;
        e.coverage.push_back(std::move(cov));
    }
    return e;
}

}  // namespace

void FontIndex::start(std::function<void()> on_ready, std::vector<fs::path> dirs, fs::path cache) {
    if (state_) return;  // already started
    state_ = std::make_shared<State>();
    if (dirs.empty()) {
        dirs = system_font_dirs();
        if (cache.empty()) cache = default_cache_file();
    }
    // Detached: the thread keeps the state alive through its own shared_ptr,
    // so quitting the app while it still scans is harmless.
    std::thread([state = state_, dirs = std::move(dirs), cache = std::move(cache), on_ready = std::move(on_ready)] {
        std::vector<FileEntry> entries;
        bool changed = true;  // does the saved index need writing?
        try {
            trace::Stopwatch total;
            // 1. The font files and their size and time (the folders are walked, the files are not opened).
            struct Found {
                fs::path path;
                FileKey key;
            };
            std::vector<Found> files;
            for (const fs::path& dir : dirs) {
                std::error_code ec;
#ifdef _WIN32
                for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
#else
                for (fs::recursive_directory_iterator it(dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec), end;
                     !ec && it != end; it.increment(ec)) {
#endif
                    const std::string ext = lower(to_utf8(it->path().extension()));
                    if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
                    std::error_code e1, e2;
                    const uint64_t size = uint64_t(fs::file_size(it->path(), e1));
                    const auto mtime = fs::last_write_time(it->path(), e2).time_since_epoch().count();
                    files.push_back({it->path(), {to_utf8(it->path()), e1 ? 0 : size, e2 ? 0 : int64_t(mtime)}});
                }
            }
            const double t_walk = total.ms();
            // 2. The saved index: what is still the same file is taken from it, the rest is read.
            std::map<std::string, FileEntry> saved;
            if (!cache.empty()) saved = load_cache(cache);
            const double t_load = total.ms();
            size_t reused = 0, read = 0;
            for (const Found& f : files) {
                auto it = saved.find(f.key.path);
                if (it != saved.end() && it->second.key == f.key) {
                    entries.push_back(std::move(it->second));
                    reused++;
                } else {
                    entries.push_back(read_entry(f.path, f.key));
                    read++;
                }
            }
            changed = read > 0 || saved.size() != reused;  // (a font that was removed also changes the index)
            const double t_read = total.ms();
            // 3. The index in memory.
            std::vector<FontFace> faces;
            for (const FileEntry& e : entries) {
                for (size_t i = 0; i < e.faces.size(); i++) {
                    faces.push_back(e.faces[i]);
                    state->coverage[{e.faces[i].file, e.faces[i].face}] = e.coverage[i];
                }
            }
            std::sort(faces.begin(), faces.end(), [](const FontFace& a, const FontFace& b) {
                return a.file != b.file ? a.file < b.file : a.face < b.face;
            });
            state->families = list_families(faces);
            state->faces = std::move(faces);
            trace::log("font index: %zu files, %zu faces; walk %.1f ms, load saved index %.1f ms (%s), read %zu files %.1f ms, build %.1f ms, total %.1f ms",
                       files.size(), state->faces.size(), t_walk, t_load - t_walk, saved.empty() ? "none" : "found", read, t_read - t_load, total.ms() - t_read, total.ms());
        } catch (...) {
            // A scan that fails leaves an empty index: named fonts are then "not found".
            changed = false;
        }
        state->ready.store(true, std::memory_order_release);
        if (on_ready) on_ready();
        // Saving comes after the index is usable: nothing waits for it.
        if (changed && !cache.empty() && !entries.empty()) {
            try {
                trace::Stopwatch sw;
                save_cache(cache, entries);
                trace::log("font index: saved %s in %.1f ms", to_utf8(cache).c_str(), sw.ms());
            } catch (...) {
            }
        }
    }).detach();
}

bool FontIndex::wait(double seconds) const {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (state_ && !ready() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return ready();
}

const std::vector<FontFace>& FontIndex::faces() const {
    static const std::vector<FontFace> none;
    return ready() ? state_->faces : none;
}

const std::vector<FontFamily>& FontIndex::families() const {
    static const std::vector<FontFamily> none;
    return ready() ? state_->families : none;
}

const FaceCoverage* FontIndex::coverage(const FontFace& face) const {
    if (!ready()) return nullptr;
    auto it = state_->coverage.find({face.file, face.face});
    return it == state_->coverage.end() ? nullptr : &it->second;
}
