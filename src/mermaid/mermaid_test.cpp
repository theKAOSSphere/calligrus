// Test runner for the C++ Mermaid port (console, no window).
//
//   calligrus-mermaid-test [--verbose] [name-filter]
//   CALLIGRUS_MERMAID_MASKS=<dir> [CALLIGRUS_MERMAID_SCALE=1.05] additionally compares the native
//   rasteriser with the Rust path's masks (made by `mermaid-oracle --masks <dir> <scale> ...`).
//
// For every tests/mermaid/*.mmd it compares our output with the goldens written by
// scripts/mermaid-oracle (the Rust crate + fix_svg, exactly what the app feeds to resvg):
//   * NODES  : node sizes and relative positions against X.layout.txt (stage test)
//   * SVG    : byte-identical to X.svg = "identical"; same text with numbers within 0.01 =
//              "numeric"; anything else prints the first differing position.
// A diagram the port cannot handle yet reports "unsupported (reason)": that is not a failure,
// it is what makes the app fall back to the Rust renderer. Exit code 1 only on crashes or
// regressions from a "known good" list (kMustBeIdentical).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "fontmetrics.h"
#include "layout.h"
#include "native.h"
#include "numfmt.h"
#include "parser.h"
#include "render.h"
#include "strutil.h"

namespace fs = std::filesystem;
using namespace mermaid;

namespace {

// Diagrams that already match byte for byte: a regression here fails the test run.
const std::vector<std::string> kMustBeIdentical = {"f01-min", "f02-lr", "f03-rl", "f04-bt", "f05-shapes", "f09-chain", "f10-fanout", "f15-styles", "f16-long-labels", "f17-self-loop", "f18-multi-edge"};

// Inputs the Rust crate rejects but Calligrus draws on purpose (a Calligrus deviation; PORTING.md, "Deviations").
const std::vector<std::string> kRendersWhereRustRejects = {"class-a04-stereo"};

// Sankey goldens whose layout Calligrus deliberately does differently (d3-sankey like mermaid.js; PORTING.md, "Deviations").
const std::vector<std::string> kSankeyDeviates = {"sankey-torture", "sankey-t-basic", "exp-sankey-energy", "bench-sankey-medium"};

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<uint8_t> read_bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- SVG comparison ----

struct Token {
    bool is_number;
    std::string text;
    double value;
};

// Split into runs of text and numbers ("-12.50" is a number).
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0;
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    while (i < s.size()) {
        size_t j = i;
        bool num = is_digit(s[i]) || (s[i] == '-' && i + 1 < s.size() && is_digit(s[i + 1]));
        if (num) {
            ++j;
            while (j < s.size() && (is_digit(s[j]) || s[j] == '.')) ++j;
            out.push_back({true, s.substr(i, j - i), std::strtod(s.substr(i, j - i).c_str(), nullptr)});
        } else {
            while (j < s.size() && !(is_digit(s[j]) || (s[j] == '-' && j + 1 < s.size() && is_digit(s[j + 1])))) ++j;
            out.push_back({false, s.substr(i, j - i), 0});
        }
        i = j;
    }
    return out;
}

bool numerically_equal(const std::string& a, const std::string& b, double tol) {
    auto ta = tokenize(a), tb = tokenize(b);
    if (ta.size() != tb.size()) return false;
    for (size_t i = 0; i < ta.size(); ++i) {
        if (ta[i].is_number != tb[i].is_number) return false;
        if (ta[i].is_number) {
            if (std::fabs(ta[i].value - tb[i].value) > tol) return false;
        } else if (ta[i].text != tb[i].text) {
            return false;
        }
    }
    return true;
}

std::string first_difference(const std::string& got, const std::string& want) {
    size_t n = std::min(got.size(), want.size());
    size_t i = 0;
    while (i < n && got[i] == want[i]) ++i;
    auto ctx = [&](const std::string& s) {
        size_t a = i > 40 ? i - 40 : 0;
        return s.substr(a, 90);
    };
    std::ostringstream o;
    o << "first difference at byte " << i << " (got " << got.size() << " bytes, want " << want.size() << ")\n"
      << "      got : ..." << ctx(got) << "\n"
      << "      want: ..." << ctx(want);
    return o.str();
}

// ---- layout stage comparison (X.layout.txt, "node" lines) ----

struct NodeBox {
    float x, y, w, h;
};

std::map<std::string, NodeBox> parse_layout_nodes(const std::string& txt) {
    std::map<std::string, NodeBox> m;
    for (const std::string& line : lines_of(txt)) {
        if (!starts_with(line, "node ")) continue;
        auto parts = split_whitespace(line);
        if (parts.size() < 6) continue;
        // The node id may contain spaces: it ends where four numbers (x y w h) start.
        auto is_num = [](const std::string& t) {
            char* end = nullptr;
            std::strtof(t.c_str(), &end);
            return !t.empty() && *end == 0;
        };
        size_t i = 2;
        while (i + 4 < parts.size() && !(is_num(parts[i]) && is_num(parts[i + 1]) && is_num(parts[i + 2]) && is_num(parts[i + 3]))) ++i;
        std::string id = parts[1];
        for (size_t k = 2; k < i; ++k) id += " " + parts[k];
        m[id] = {std::strtof(parts[i].c_str(), nullptr), std::strtof(parts[i + 1].c_str(), nullptr),
                 std::strtof(parts[i + 2].c_str(), nullptr), std::strtof(parts[i + 3].c_str(), nullptr)};
    }
    return m;
}

// Compare sizes exactly and positions relative to the top-left-most node (the Rust layout is
// translated by edge geometry we have not produced yet). Returns a message ("" = match).
std::string compare_nodes(const Layout& got, const std::map<std::string, NodeBox>& want) {
    if (got.nodes.size() != want.size()) return "node count differs";
    float gx = 1e30f, gy = 1e30f, wx = 1e30f, wy = 1e30f;
    for (const auto& [id, n] : got.nodes) { gx = std::min(gx, n.x); gy = std::min(gy, n.y); }
    for (const auto& [id, b] : want) { wx = std::min(wx, b.x); wy = std::min(wy, b.y); }
    double worst = 0;
    std::string worst_id;
    for (const auto& [id, n] : got.nodes) {
        auto it = want.find(id);
        if (it == want.end()) return "unknown node " + id;
        const NodeBox& b = it->second;
        double d = std::max({std::fabs(double(n.width) - b.w), std::fabs(double(n.height) - b.h),
                             std::fabs(double(n.x - gx) - double(b.x - wx)), std::fabs(double(n.y - gy) - double(b.y - wy))});
        if (d > worst) { worst = d; worst_id = id; }
    }
    if (worst > 0.002) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "max deviation %.4f at node %s", worst, worst_id.c_str());
        return buf;
    }
    return "";
}


// ---- full layout dump, same text format as scripts/mermaid-oracle's X.layout.txt ----

std::string rust_debug_str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

// Rust `{:?}` of an f32: like `{}` but whole numbers keep a ".0".
std::string debug_f32(float v) {
    std::string s = fmt_shortest(v);
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}

std::string layout_to_text(const Layout& l) {
    std::ostringstream o;
    o << "size " << fmt_shortest(l.width) << " " << fmt_shortest(l.height) << "\n";
    for (const auto& [id, n] : l.nodes) {
        o << "node " << n.id << " " << fmt_shortest(n.x) << " " << fmt_shortest(n.y) << " " << fmt_shortest(n.width) << " "
          << fmt_shortest(n.height) << " hidden=" << (n.hidden ? "true" : "false") << " lines=[";
        for (size_t i = 0; i < n.label.lines.size(); ++i) o << (i ? ", " : "") << rust_debug_str(n.label.lines[i]);
        o << "] label=" << fmt_shortest(n.label.width) << "x" << fmt_shortest(n.label.height) << "\n";
    }
    for (const auto& e : l.edges) {
        o << "edge " << e.from << "->" << e.to << " ";
        for (size_t i = 0; i < e.points.size(); ++i)
            o << (i ? " " : "") << "(" << fmt_shortest(e.points[i].first) << "," << fmt_shortest(e.points[i].second) << ")";
        o << " anchor=";
        if (e.label_anchor)
            o << "Some((" << debug_f32(e.label_anchor->first) << ", " << debug_f32(e.label_anchor->second) << "))";
        else
            o << "None";
        o << "\n";
    }
    for (const auto& sg : l.subgraphs)
        o << "sub " << rust_debug_str(sg.label) << " " << fmt_shortest(sg.x) << " " << fmt_shortest(sg.y) << " "
          << fmt_shortest(sg.width) << " " << fmt_shortest(sg.height) << "\n";
    return o.str();
}

std::string first_line_difference(const std::string& got, const std::string& want) {
    auto g = lines_of(got), w = lines_of(want);
    for (size_t i = 0; i < std::max(g.size(), w.size()); ++i) {
        std::string a = i < g.size() ? g[i] : "<missing>", b = i < w.size() ? w[i] : "<missing>";
        if (a != b) return "line " + std::to_string(i + 1) + "\n      got : " + a + "\n      want: " + b;
    }
    return "";
}

// ---- rasteriser comparison against masks written by `mermaid-oracle --masks` ----

struct MaskStats {
    bool size_equal = false;
    size_t pixels = 0, differ = 0, big = 0;  // big: differs by more than 32 of 255
    int max_diff = 0;
    double mean_abs = 0;
};

bool read_mask(const fs::path& p, uint32_t& w, uint32_t& h, std::vector<uint8_t>& ink) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    uint32_t hdr[2];
    f.read(reinterpret_cast<char*>(hdr), 8);
    if (!f) return false;
    w = hdr[0];
    h = hdr[1];
    ink.resize(size_t(w) * h);
    f.read(reinterpret_cast<char*>(ink.data()), static_cast<std::streamsize>(ink.size()));
    return static_cast<bool>(f);
}

MaskStats compare_mask(const InkImage& got, uint32_t w, uint32_t h, const std::vector<uint8_t>& want) {
    MaskStats st;
    st.size_equal = got.width == w && got.height == h;
    if (!st.size_equal) return st;
    st.pixels = want.size();
    double sum = 0;
    for (size_t i = 0; i < want.size(); ++i) {
        int d = std::abs(int(got.rgba[i * 4]) - int(want[i]));
        if (d) ++st.differ;
        if (d > 32) ++st.big;
        st.max_diff = std::max(st.max_diff, d);
        sum += d;
    }
    st.mean_abs = sum / double(std::max<size_t>(want.size(), 1));
    return st;
}

// Writes a greyscale PGM (ink as darkness) so a picture can be looked at.
void write_pgm(const fs::path& p, uint32_t w, uint32_t h, const std::vector<uint8_t>& gray) {
    std::ofstream f(p, std::ios::binary);
    f << "P5\n" << w << " " << h << "\n255\n";
    f.write(reinterpret_cast<const char*>(gray.data()), static_cast<std::streamsize>(gray.size()));
}

// ---- Calligrus deviations from mermaid-rs-renderer (each one is listed in PORTING.md, "Deviations") ----
// These cannot come from the oracle (the reference lacks the feature), so they are checked here by other means.

int check_failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << "  DEVIATION " << (ok ? "ok    " : "FAILED ") << what << "\n";
    if (!ok) ++check_failures;
}

size_t ink_sum(const InkImage& img) {
    size_t sum = 0;
    for (size_t i = 0; i < img.rgba.size(); i += 4) sum += img.rgba[i];
    return sum;
}

void run_deviation_tests() {
    std::cout << "deviations\n";

    // 1. `<<interface>>` inside a class body. The reference rejects the line as "an arrow without a source"; Mermaid itself takes
    // it, and so do we: the result must equal the reference's own output for the same class with `R : <<interface>>`.
    {
        const std::string body_form = R"(classDiagram
    class DocState {
        +path: fs::path
        +doc: md::Document
        +cache: LayoutCache
        +load(path) bool
        -too_large_note() string
    }
    class Tab {
        +st: DocState
        +slides: Slides
    }
    class Renderer {
        <<interface>>
        +draw(Block&) void
    }
    Tab "1" *-- "1" DocState
    DocState o-- "many" Block
    Block <|-- Paragraph
    Block <|-- CodeBlock
    Block <|-- MathBlock
    Renderer <|.. GpuRenderer
    Renderer <|.. PdfRenderer
)";
        const std::string want = read_file(fs::path(CALLIGRUS_MERMAID_CORPUS) / "class-stereotype-member.svg");
        std::string got;
        try { got = render_mermaid_svg(body_form); } catch (const std::exception& e) { got = std::string("error: ") + e.what(); }
        check(!want.empty() && got == want, "class: <<interface>> in a body = the reference's `R : <<interface>>` (byte-identical SVG)");
        // a real leading arrow is still rejected
        bool rejected = false;
        try { render_mermaid_svg("flowchart LR\n  --> B\n"); } catch (const ParseError&) { rejected = true; }
        check(rejected, "a line starting with a real arrow is still rejected");
    }

    // 2. sankey keeps its colours: a colour layer next to the ink mask; other kinds do not.
    {
        const std::string sankey = read_file(fs::path(CALLIGRUS_MERMAID_CORPUS) / "sankey-torture.mmd");
        bool keep = false;
        render_mermaid_svg(sankey, &keep);
        check(keep, "sankey asks for its colours");
        bool keep_flow = true;
        render_mermaid_svg("flowchart LR\n  A --> B\n", &keep_flow);
        check(!keep_flow, "a flowchart does not");
        auto img = native_render(sankey, 1.05f);
        bool has_blue = false, has_orange = false;
        size_t coloured = 0;
        if (img && img->colour.size() == img->rgba.size()) {
            for (size_t i = 0; i < img->colour.size(); i += 4) {
                const uint8_t r = img->colour[i], g = img->colour[i + 1], b = img->colour[i + 2], a = img->colour[i + 3];
                if (a == 255) {
                    ++coloured;
                    if (r == 0x4e && g == 0x79 && b == 0xa7) has_blue = true;    // node "Input"
                    if (r == 0xf2 && g == 0x8e && b == 0x2c) has_orange = true;  // node "Org"
                }
            }
        }
        check(img && has_blue && has_orange && coloured > 1000, "sankey colour layer holds the palette fills (#4e79a7, #f28e2c)");
        check(img && ink_sum(*img) > 0, "sankey ink layer still holds the black labels");
        auto flow = native_render("flowchart LR\n  A --> B\n", 1.05f);
        check(flow && flow->colour.empty(), "flowchart has no colour layer (reference behaviour)");
    }

    // 2b. sankey layout like d3-sankey (mermaid.js): the crate stacks the nodes of a column edge to edge and puts the bands at the
    // source's top. Checked on the whole sankey corpus: gaps >= the padding, bands inside their nodes, heights = flow sums.
    {
        const char* const names[] = {"sankey-torture", "sankey-t-basic", "exp-sankey-energy", "bench-sankey-medium"};
        for (const char* name : names) {
            const std::string src = read_file(fs::path(CALLIGRUS_MERMAID_CORPUS) / (std::string(name) + ".mmd"));
            Theme theme;
            LayoutConfig config;
            Layout l = compute_layout(parse_mermaid(src), theme, config);
            bool gaps_ok = true, ends_ok = true, heights_ok = true;
            if (!l.sankey) { check(false, std::string(name) + ": sankey data present"); continue; }
            const SankeyData& sk = *l.sankey;
            std::map<std::string, const SankeyNodeLayout*> by_id;
            std::map<std::string, std::vector<const SankeyNodeLayout*>> columns;
            for (const auto& n : sk.nodes) {
                by_id[n.id] = &n;
                columns[std::to_string(n.rank)].push_back(&n);
            }
            for (auto& [rank, col] : columns) {
                std::sort(col.begin(), col.end(), [](auto a, auto b) { return a->y < b->y; });
                if (!col.empty() && col.front()->y < -0.01f) gaps_ok = false;
                for (size_t i = 0; i + 1 < col.size(); ++i)
                    if (col[i + 1]->y - (col[i]->y + col[i]->height) < 23.99f) gaps_ok = false;
                if (!col.empty() && col.back()->y + col.back()->height > sk.height + 0.01f) gaps_ok = false;
            }
            std::map<std::string, float> in_sum, out_sum;
            for (const auto& k : sk.links) {
                const SankeyNodeLayout &s = *by_id[k.source], &t = *by_id[k.target];
                if (k.start.second - k.thickness / 2 < s.y - 0.01f || k.start.second + k.thickness / 2 > s.y + s.height + 0.01f) ends_ok = false;
                if (k.end.second - k.thickness / 2 < t.y - 0.01f || k.end.second + k.thickness / 2 > t.y + t.height + 0.01f) ends_ok = false;
                out_sum[k.source] += k.thickness;
                in_sum[k.target] += k.thickness;
            }
            for (const auto& n : sk.nodes)
                if (std::fabs(n.height - std::max(in_sum[n.id], out_sum[n.id])) > 0.01f) heights_ok = false;
            check(gaps_ok, std::string(name) + ": nodes of a column are at least the padding (24) apart and inside the diagram");
            check(ends_ok, std::string(name) + ": every link starts inside its source node and ends inside its target node");
            check(heights_ok, std::string(name) + ": node height = max(incoming, outgoing) flow");
        }
    }

    // 3. maths in labels, through a stand-in TeX engine (the console test has no math font): marker -> sized box -> data-tex
    // -> bitmap; and without an engine (or when it refuses) the reference's plain-text approximation, byte for byte.
    {
        const std::string src = "flowchart LR\n  A[\"$$x^2$$\"] --> B[\"plain\"]\n";
        const std::string plain_svg = render_mermaid_svg(src);
        check(plain_svg.find("data-tex") == std::string::npos, "no TeX engine: the label is the plain-text approximation");
        const InkImage plain_img = *native_render(src, 1.05f);

        MathHooks fake;
        fake.measure = [](const std::string& tex, float fs) -> std::optional<MathBox> {
            if (tex.find("bad") != std::string::npos) return std::nullopt;
            return MathBox{6.0f * float(tex.size()) * fs / 16.0f, 30.0f * fs / 16.0f, 9.0f * fs / 16.0f};
        };
        fake.render = [](const std::string&, float ppem, MathBitmap& out) {
            out.width = uint32_t(std::lround(ppem * 4));
            out.height = uint32_t(std::lround(ppem * 2.5f));
            out.rgba.assign(size_t(out.width) * out.height * 4, 255);  // a solid block of ink
            out.origin_x = 0;
            out.origin_y = ppem * 1.9f;
            return true;
        };
        set_math_hooks(fake);
        const std::string tex_svg = render_mermaid_svg(src);
        check(tex_svg.find("data-tex=\"x^2\"") != std::string::npos, "TeX engine: the label line is marked data-tex (plain text kept as fallback)");
        Layout with = compute_layout(parse_mermaid(src), Theme{}, LayoutConfig{});
        set_math_hooks({});
        Layout without = compute_layout(parse_mermaid(src), Theme{}, LayoutConfig{});
        check(with.nodes.at("A").height > without.nodes.at("A").height, "TeX engine: the node is sized from the formula's box (taller than the text one)");
        check(with.nodes.at("B").height == without.nodes.at("B").height, "text-only labels are sized as before");
        set_math_hooks(fake);
        const InkImage tex_img = *native_render(src, 1.05f);
        check(ink_sum(tex_img) != ink_sum(plain_img), "TeX engine: the formula bitmap is drawn in the label");
        // a formula the engine cannot typeset keeps the approximation
        const std::string bad = render_mermaid_svg("flowchart LR\n  A[\"$$bad$$\"]\n");
        check(bad.find("data-tex") == std::string::npos, "unparseable formula: falls back to plain text");
        // a formula mixed with other text on its line is not a TeX label (plain approximation, as in the reference)
        const std::string mixed = render_mermaid_svg("flowchart LR\n  A[\"area $$x^2$$\"]\n");
        check(mixed.find("data-tex") == std::string::npos, "formula inside a sentence: plain text");
        // the engine refuses to draw: the tspan's plain text is drawn
        MathHooks refuses = fake;
        refuses.render = [](const std::string&, float, MathBitmap&) { return false; };
        set_math_hooks(refuses);
        const InkImage fallback_img = *native_render(src, 1.05f);
        check(fallback_img.width > 0 && ink_sum(fallback_img) > 0 && ink_sum(fallback_img) != ink_sum(tex_img), "engine refuses to draw: the plain text is drawn instead");
        set_math_hooks({});
        const std::string again = render_mermaid_svg(src);
        check(again == plain_svg, "hooks cleared: back to the reference's output");
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--verbose" || a == "-v") verbose = true;
        else filter = a;
    }

    FontMetrics font;
    if (!font.load(read_bytes(CALLIGRUS_MERMAID_FONT))) {
        std::cerr << "cannot load font " << CALLIGRUS_MERMAID_FONT << "\n";
        return 2;
    }
    // The native renderer (layout measuring + drawing) uses the same bundled faces.
    const char* masks_env = std::getenv("CALLIGRUS_MERMAID_MASKS");
    const char* dump_env = std::getenv("CALLIGRUS_MERMAID_DUMP");
    const char* scale_env = std::getenv("CALLIGRUS_MERMAID_SCALE");
    float raster_scale = scale_env ? std::strtof(scale_env, nullptr) : 1.05f;
    fs::path font_dir = fs::path(CALLIGRUS_MERMAID_FONT).parent_path();
    NativeFonts nf;
    nf.regular = read_bytes(CALLIGRUS_MERMAID_FONT);
    nf.bold = read_bytes((font_dir / "IBMPlexSans-Bold.ttf").string());
    nf.italic = read_bytes((font_dir / "IBMPlexSans-Italic.ttf").string());
    if (!native_init(std::move(nf))) {
        std::cerr << "cannot initialise the native renderer\n";
        return 2;
    }
    int raster_done = 0, raster_unsup = 0, raster_missing = 0;
    double pct_sum = 0, pct_max = 0;

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(CALLIGRUS_MERMAID_CORPUS))
        if (e.path().extension() == ".mmd" && e.path().filename().string().find(filter) != std::string::npos)
            files.push_back(e.path());
    std::sort(files.begin(), files.end());

    Theme theme;
    LayoutConfig config;
    int identical = 0, numeric = 0, different = 0, unsupported = 0, nodes_ok = 0, nodes_bad = 0, nodes_unsup = 0;
    int rejected_ok = 0;
    int regressions = 0, layout_ok = 0, layout_bad = 0, layout_unsup = 0;
    std::vector<std::string> known_good_now;

    for (const fs::path& mmd : files) {
        std::string name = mmd.stem().string();
        std::string src = read_file(mmd);
        std::cout << name << "\n";

        // No golden SVG = the Rust crate rejects this input (a ParseError): we must reject it as well.
        {
            fs::path golden_path = mmd;
            golden_path.replace_extension(".svg");
            if (!fs::exists(golden_path)) {
                try {
                    render_mermaid_svg(src);
                    if (std::find(kRendersWhereRustRejects.begin(), kRendersWhereRustRejects.end(), name) != kRendersWhereRustRejects.end()) {
                        ++rejected_ok;
                        std::cout << "  renders although the Rust crate rejects it (known deviation, see PORTING.md)\n";
                        continue;
                    }
                    ++different;
                    std::cout << "  both-reject DIFFERENT: the Rust crate rejects this input but the port renders it\n";
                } catch (const ParseError& e) {
                    ++rejected_ok;
                    std::cout << "  rejected like Rust (" << e.what() << ")\n";
                } catch (const std::exception& e) {
                    ++different;
                    std::cout << "  both-reject DIFFERENT: " << e.what() << "\n";
                }
                continue;
            }
        }

        // Calligrus lays sankeys out like mermaid.js, not like the crate: nothing to compare with (checked in run_deviation_tests).
        if (std::find(kSankeyDeviates.begin(), kSankeyDeviates.end(), name) != kSankeyDeviates.end()) {
            std::cout << "  lays out like mermaid.js, not like the crate (known deviation, see PORTING.md)\n";
            continue;
        }

        // Stage: nodes
        try {
            Graph g = parse_mermaid(src);
            Layout l = compute_layout(g, theme, config, LayoutStage::NodesOnly);
            fs::path lt = mmd;
            lt.replace_extension(".layout.txt");
            std::string msg = compare_nodes(l, parse_layout_nodes(read_file(lt)));
            if (msg.empty()) { ++nodes_ok; std::cout << "  NODES  ok\n"; }
            else { ++nodes_bad; std::cout << "  NODES  DIFFERENT: " << msg << "\n"; }
        } catch (const Unsupported& e) {
            ++nodes_unsup;
            std::cout << "  NODES  unsupported (" << e.what() << ")\n";
        } catch (const std::exception& e) {
            ++nodes_bad;
            std::cout << "  NODES  ERROR " << e.what() << "\n";
        }

        // Stage: full layout (nodes + routed edges + label anchors), text identical to the oracle
        try {
            Graph g = parse_mermaid(src);
            Layout l = compute_layout(g, theme, config);
            fs::path lt = mmd;
            lt.replace_extension(".layout.txt");
            std::string got = layout_to_text(l), want_txt = read_file(lt);
            std::string diff = got == want_txt ? "" : first_line_difference(got, want_txt);
            if (diff.empty()) { ++layout_ok; std::cout << "  LAYOUT identical\n"; }
            else { ++layout_bad; std::cout << "  LAYOUT DIFFERENT at " << diff << "\n"; }
        } catch (const Unsupported& e) {
            ++layout_unsup;
            std::cout << "  LAYOUT unsupported (" << e.what() << ")\n";
        } catch (const std::exception& e) {
            ++layout_bad;
            std::cout << "  LAYOUT ERROR " << e.what() << "\n";
        }

        // Stage: SVG
        fs::path golden = mmd;
        golden.replace_extension(".svg");
        std::string want = read_file(golden);
        try {
            std::string got = render_mermaid_svg(src);
            if (got == want) {
                ++identical;
                known_good_now.push_back(name);
                std::cout << "  SVG    identical\n";
            } else if (numerically_equal(got, want, 0.01)) {
                ++numeric;
                std::cout << "  SVG    numeric (equal within 0.01, not byte-identical)\n";
                if (verbose) std::cout << "    " << first_difference(got, want) << "\n";
            } else {
                ++different;
                std::cout << "  SVG    DIFFERENT: " << first_difference(got, want) << "\n";
            }
        } catch (const Unsupported& e) {
            ++unsupported;
            std::cout << "  SVG    unsupported (" << e.what() << ")\n";
        } catch (const std::exception& e) {
            ++different;
            std::cout << "  SVG    ERROR " << e.what() << "\n";
        }

        // Stage: rasteriser (only when masks from the Rust path are available)
        if (masks_env) {
            uint32_t mw = 0, mh = 0;
            std::vector<uint8_t> want_ink;
            if (!read_mask(fs::path(masks_env) / (name + ".mask"), mw, mh, want_ink)) {
                ++raster_missing;
                std::cout << "  RASTER no mask\n";
            } else if (auto img = native_render(src, raster_scale)) {
                MaskStats ms = compare_mask(*img, mw, mh, want_ink);
                ++raster_done;
                if (!ms.size_equal) {
                    std::cout << "  RASTER SIZE DIFFERENT got " << img->width << "x" << img->height << " want " << mw << "x" << mh << "\n";
                } else {
                    double pct = 100.0 * double(ms.differ) / double(std::max<size_t>(ms.pixels, 1));
                    pct_sum += pct;
                    pct_max = std::max(pct_max, pct);
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "  RASTER %zu of %zu px differ (%.3f%%), >32: %.3f%%, max %d, mean abs %.4f", ms.differ,
                                  ms.pixels, pct, 100.0 * double(ms.big) / double(std::max<size_t>(ms.pixels, 1)), ms.max_diff, ms.mean_abs);
                    std::cout << buf << "\n";
                }
                if (dump_env) {
                    fs::create_directories(dump_env);
                    std::vector<uint8_t> g(size_t(img->width) * img->height);
                    for (size_t i = 0; i < g.size(); ++i) g[i] = static_cast<uint8_t>(255 - img->rgba[i * 4]);
                    write_pgm(fs::path(dump_env) / (name + ".native.pgm"), img->width, img->height, g);
                    if (ms.size_equal) {
                        std::vector<uint8_t> w2(want_ink.size()), d(want_ink.size());
                        for (size_t i = 0; i < w2.size(); ++i) {
                            w2[i] = static_cast<uint8_t>(255 - want_ink[i]);
                            d[i] = static_cast<uint8_t>(255 - std::min(255, 4 * std::abs(int(img->rgba[i * 4]) - int(want_ink[i]))));
                        }
                        write_pgm(fs::path(dump_env) / (name + ".rust.pgm"), mw, mh, w2);
                        write_pgm(fs::path(dump_env) / (name + ".diff.pgm"), mw, mh, d);
                    }
                }
            } else {
                ++raster_unsup;
                std::cout << "  RASTER native unsupported (" << native_last_reason() << ")\n";
            }
        }
    }

    if (masks_env)
        std::cout << "\nRASTER: " << raster_done << " compared (mean " << (raster_done ? pct_sum / raster_done : 0.0) << "% of pixels differ, worst "
                  << pct_max << "%), " << raster_unsup << " unsupported, " << raster_missing << " without mask\n";

    if (filter.empty())
        for (const std::string& must : kMustBeIdentical)
            if (std::find(known_good_now.begin(), known_good_now.end(), must) == known_good_now.end()) {
                std::cout << "REGRESSION: " << must << " is no longer byte-identical\n";
                ++regressions;
            }

    if (filter.empty() || filter == "deviations") run_deviation_tests();
    regressions += check_failures;

    std::cout << "\n" << files.size() << " diagrams (" << rejected_ok << " rejected like Rust): SVG identical " << identical << ", numeric " << numeric
              << ", different " << different << ", unsupported " << unsupported << " | nodes ok " << nodes_ok
              << ", bad " << nodes_bad << ", unsupported " << nodes_unsup << "\n";
    // Anything that renders must match the oracle byte for byte (layout text too); only "unsupported" is allowed.
    return (regressions == 0 && nodes_bad == 0 && different == 0 && layout_bad == 0) ? 0 : 1;
}
