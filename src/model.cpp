// md4c -> Block/Span tree.
//
// md4c is a SAX-style parser: instead of handing us a tree it calls our
// functions as it walks the text ("entering a paragraph", "here is some text",
// "leaving the paragraph"...). We keep a small stack of "where do new blocks go"
// frames and build the tree as the callbacks arrive.
#include "model.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <md4c.h>

namespace md {
namespace {

// One open container (document, quote, list, list item).
struct Frame {
    Block* owner = nullptr;              // the Quote/List block, or nullptr
    std::vector<Block>* out = nullptr;   // where child blocks are appended
    bool loose_para = false;             // see Builder::target()
};

struct Builder {
    Document doc;
    std::vector<Frame> stack;

    Spans* inlines = nullptr;            // current paragraph/heading/cell
    std::string* code = nullptr;         // current code block body
    Block* table = nullptr;
    bool in_thead = false;
    bool html_comment = false;           // inside <!-- ... --> spanning several HTML chunks
    bool html_block = false;             // inside a raw HTML block (not inline HTML)
    const char* raw_close = nullptr;     // inside a hidden <style>/<script> block: the tag that ends it (spec 31)
    std::vector<int> hidden_start;       // first line (0-based) of each hidden top-level <style>/<script> block

    // Inline style state, toggled by enter/leave span.
    bool bold = false, italic = false, strike = false, code_span = false, math = false;
    bool display_math = false;
    std::string link;
    std::string image;                   // inside ![..](src): alt-text spans get tagged with src

    // Source positions (for editor scroll sync): md4c hands out pointers into
    // the text it parses, so the first pointer seen inside a top-level block
    // tells which line that block starts on.
    const char* base = nullptr;
    size_t base_size = 0;
    std::vector<size_t> line_starts;     // offset of each line of the parsed text
    std::vector<int> top_start;          // per top-level block: first line (0-based), -1 = no pointer seen
    std::vector<char> top_is_code;       // per top-level block: a code block
    int cur_top = -1;                    // index of the open top-level block
    // The same one level down: the items of a top-level list, the children of a top-level quote.
    std::vector<std::vector<int>> sub_start;    // per top-level block, per item/child: first line, -1 = no pointer seen
    std::vector<std::vector<char>> sub_code;    // ...and whether it opens with a code block
    int cur_sub = -1;                    // index of the open item/child within cur_top

    void add_sub() {
        if (cur_top < 0) return;
        sub_start.resize(size_t(cur_top) + 1);
        sub_code.resize(size_t(cur_top) + 1);
        sub_start[size_t(cur_top)].push_back(-1);
        sub_code[size_t(cur_top)].push_back(0);
        cur_sub = int(sub_start[size_t(cur_top)].size()) - 1;
    }

    int line_of(const char* p) const {
        if (!p || p < base || p >= base + base_size) return -1;
        const size_t off = size_t(p - base);
        return int(std::upper_bound(line_starts.begin(), line_starts.end(), off) - line_starts.begin()) - 1;
    }
    void seen(const char* p) {
        if (cur_top < 0) return;
        const bool need_top = top_start[size_t(cur_top)] < 0;
        const bool need_sub = cur_sub >= 0 && sub_start[size_t(cur_top)][size_t(cur_sub)] < 0;
        if (!need_top && !need_sub) return;
        const int l = line_of(p);
        if (l < 0) return;
        if (need_top) top_start[size_t(cur_top)] = l;
        if (need_sub) sub_start[size_t(cur_top)][size_t(cur_sub)] = l;
    }

    Block& push(Block::Kind kind) {
        Frame& f = stack.back();
        f.loose_para = false;  // any new block ends a run of loose text
        f.out->push_back(Block{});
        f.out->back().kind = kind;
        if (stack.size() == 1) {  // a top-level block
            cur_top = int(f.out->size()) - 1;
            top_start.resize(f.out->size(), -1);
            top_is_code.resize(f.out->size(), 0);
            top_is_code.back() = kind == Block::Kind::Code;
            cur_sub = -1;
        } else if (stack.size() == 2 && cur_top >= 0 && stack[1].owner && stack[1].owner->kind == Block::Kind::Quote) {
            add_sub();  // a child of a top-level quote
        }
        return f.out->back();
    }

    // Where inline text should go. Normally the open paragraph/heading/cell.
    // But in *tight* lists md4c gives us text directly inside <li> with no <p>
    // around it, so we open an implicit paragraph to hold it.
    Spans* target() {
        if (inlines) return inlines;
        if (stack.empty()) return nullptr;
        Frame& f = stack.back();
        if (!f.out) return nullptr;
        if (!f.loose_para) {
            push(Block::Kind::Paragraph);
            f.loose_para = true;
        }
        return &f.out->back().spans;
    }

    void add_text(std::string_view text, bool as_code = false) {
        Spans* t = target();
        if (!t) return;
        // md4c hands a multi-line $$..$$ over in pieces (one per line): glue them back together.
        if (math && display_math && !t->empty() && t->back().math && t->back().display) {
            t->back().text += text;
            return;
        }
        Span s;
        s.display = math && display_math;
        s.text = std::string(text);
        s.bold = bold;
        s.italic = italic;
        s.strike = strike;
        s.code = code_span || as_code;
        s.math = math;
        s.link = link;
        s.small = link.rfind("#fn-", 0) == 0;
        s.image_src = image;
        t->push_back(std::move(s));
    }

    // The first span of an image: carries the source (and, for <img>, the alt text).
    void add_image(std::string src, std::string alt, float width) {
        Spans* t = target();
        if (!t) return;
        Span s;
        s.text = std::move(alt);
        s.image_src = std::move(src);
        s.image_width = width;
        s.image_start = true;
        s.link = link;
        t->push_back(std::move(s));
    }

    void html(std::string_view chunk);
    bool hides_raw_html(std::string_view chunk);
};

// md4c passes attribute strings (link hrefs, code info) as non-terminated slices.
std::string attr(const MD_ATTRIBUTE& a) { return std::string(a.text, a.size); }

Align to_align(MD_ALIGN a) {
    switch (a) {
        case MD_ALIGN_LEFT: return Align::Left;
        case MD_ALIGN_CENTER: return Align::Center;
        case MD_ALIGN_RIGHT: return Align::Right;
        default: return Align::Default;
    }
}

int enter_block(MD_BLOCKTYPE type, void* detail, void* user) {
    auto& b = *static_cast<Builder*>(user);
    switch (type) {
        case MD_BLOCK_DOC:
            b.stack.push_back({nullptr, &b.doc});
            break;
        case MD_BLOCK_QUOTE: {
            Block& q = b.push(Block::Kind::Quote);
            b.stack.push_back({&q, &q.children});
            break;
        }
        case MD_BLOCK_UL:
        case MD_BLOCK_OL: {
            Block& l = b.push(Block::Kind::List);
            l.ordered = type == MD_BLOCK_OL;
            if (l.ordered) l.start = static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start;
            // A list frame holds no blocks directly, only items (out = nullptr).
            b.stack.push_back({&l, nullptr});
            break;
        }
        case MD_BLOCK_LI: {
            Block* list = b.stack.back().owner;
            if (b.stack.size() == 2) b.add_sub();  // an item of a top-level list
            list->items.emplace_back();
            b.stack.push_back({nullptr, &list->items.back()});
            auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            if (li->is_task) b.add_text(li->task_mark == ' ' ? "\xE2\x98\x90 " : "\xE2\x98\x91 ");  // ☐ / ☑ (like the Rust version)
            break;
        }
        case MD_BLOCK_HR:
            b.push(Block::Kind::Rule);
            break;
        case MD_BLOCK_H: {
            Block& h = b.push(Block::Kind::Heading);
            h.level = static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level);
            b.inlines = &h.spans;
            break;
        }
        case MD_BLOCK_CODE: {
            Block& c = b.push(Block::Kind::Code);
            if (b.cur_sub >= 0 && b.cur_top >= 0 && b.sub_start[size_t(b.cur_top)][size_t(b.cur_sub)] < 0)
                b.sub_code[size_t(b.cur_top)][size_t(b.cur_sub)] = 1;  // the item/child opens with this block
            c.lang = attr(static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->lang);
            b.seen(static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->lang.text);
            b.code = &c.text;
            break;
        }
        case MD_BLOCK_P:
            b.inlines = &b.push(Block::Kind::Paragraph).spans;
            break;
        case MD_BLOCK_HTML:
            // Raw HTML block: its text arrives as MD_TEXT_HTML and goes through
            // Builder::html() (keeps <img>/<br>, strips other tags to their text).
            b.html_block = true;
            break;
        case MD_BLOCK_TABLE:
            b.table = &b.push(Block::Kind::Table);
            break;
        case MD_BLOCK_THEAD: b.in_thead = true; break;
        case MD_BLOCK_TBODY: b.in_thead = false; break;
        case MD_BLOCK_TR:
            if (b.table && !b.in_thead) b.table->rows.emplace_back();
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            if (!b.table) break;
            auto* td = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
            if (b.in_thead) {
                b.table->header.emplace_back();
                b.table->aligns.push_back(to_align(td->align));
                b.inlines = &b.table->header.back();
            } else if (!b.table->rows.empty()) {
                b.table->rows.back().emplace_back();
                b.inlines = &b.table->rows.back().back();
            }
            break;
        }
    }
    return 0;
}

int leave_block(MD_BLOCKTYPE type, void*, void* user) {
    auto& b = *static_cast<Builder*>(user);
    switch (type) {
        case MD_BLOCK_HTML:
            b.html_block = false;
            b.raw_close = nullptr;
            break;
        case MD_BLOCK_DOC:
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
        case MD_BLOCK_LI:
            b.stack.pop_back();
            if (b.stack.size() == 1) b.cur_top = -1, b.cur_sub = -1;  // (back at the top level)
            if (b.stack.size() == 2 && type == MD_BLOCK_LI) b.cur_sub = -1;
            break;
        case MD_BLOCK_H:
        case MD_BLOCK_P:
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            b.inlines = nullptr;
            break;
        case MD_BLOCK_CODE: b.code = nullptr; break;
        case MD_BLOCK_TABLE: b.table = nullptr; break;
        default: break;
    }
    return 0;
}

int enter_span(MD_SPANTYPE type, void* detail, void* user) {
    auto& b = *static_cast<Builder*>(user);
    switch (type) {
        case MD_SPAN_EM: b.italic = true; break;
        case MD_SPAN_STRONG: b.bold = true; break;
        case MD_SPAN_DEL: b.strike = true; break;
        case MD_SPAN_CODE: b.code_span = true; break;
        case MD_SPAN_A: b.link = attr(static_cast<MD_SPAN_A_DETAIL*>(detail)->href); break;
        case MD_SPAN_LATEXMATH: b.math = true; break;
        case MD_SPAN_LATEXMATH_DISPLAY: b.math = b.display_math = true; break;
        case MD_SPAN_IMG: {
            // An empty marker span with the source; the alt text then arrives as
            // ordinary text, which add_text() tags with the same source.
            std::string src = attr(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
            b.seen(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src.text);
            b.add_image(src, "", 0);
            b.image = std::move(src);
            break;
        }
        default: break;
    }
    return 0;
}

int leave_span(MD_SPANTYPE type, void*, void* user) {
    auto& b = *static_cast<Builder*>(user);
    switch (type) {
        case MD_SPAN_EM: b.italic = false; break;
        case MD_SPAN_STRONG: b.bold = false; break;
        case MD_SPAN_DEL: b.strike = false; break;
        case MD_SPAN_CODE: b.code_span = false; break;
        case MD_SPAN_A: b.link.clear(); break;
        case MD_SPAN_LATEXMATH: b.math = false; break;
        case MD_SPAN_LATEXMATH_DISPLAY: b.math = b.display_math = false; break;
        case MD_SPAN_IMG: b.image.clear(); break;
        default: break;
    }
    return 0;
}

// The few HTML entities that commonly appear in Markdown.
std::string_view decode_entity(std::string_view e) {
    if (e == "&amp;") return "&";
    if (e == "&lt;") return "<";
    if (e == "&gt;") return ">";
    if (e == "&quot;") return "\"";
    if (e == "&#39;" || e == "&apos;") return "'";
    if (e == "&nbsp;") return " ";
    return e;
}

std::string decode_entities(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 6) {
                std::string_view e = s.substr(i, semi - i + 1);
                std::string_view d = decode_entity(e);
                if (d != e) {
                    out += d;
                    i = semi + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// Value of `name="..."` (or '...' or bare) inside a tag's text, or "".
std::string html_attr(std::string_view tag, std::string_view name) {
    std::string lower(tag);
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (size_t from = 0;;) {
        size_t at = lower.find(name, from);
        if (at == std::string::npos) return "";
        bool word_start = at == 0 || std::isspace(static_cast<unsigned char>(lower[at - 1]));
        size_t p = at + name.size();
        while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) p++;
        if (word_start && p < tag.size() && tag[p] == '=') {
            p++;
            while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) p++;
            if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) {
                size_t end = tag.find(tag[p], p + 1);
                return std::string(tag.substr(p + 1, end == std::string_view::npos ? std::string_view::npos : end - p - 1));
            }
            size_t end = p;
            while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end]))) end++;
            return std::string(tag.substr(p, end - p));
        }
        from = at + name.size();
    }
}

}  // namespace

// We don't render HTML, but the common bits are understood: <img> becomes an
// image, <br> a line break, comments vanish, and other tags are stripped to
// their text (same rules as the Rust port).
// A line of a raw HTML block that belongs to a `<style>`/`<script>` block, which is not drawn
// (CommonMark HTML block type 1: it runs to the line with the closing tag; same as model.rs).
bool Builder::hides_raw_html(std::string_view s) {
    if (!html_block) return false;  // inline <style> is left alone
    std::string lower(s);
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (raw_close) {
        if (lower.find(raw_close) != std::string::npos) raw_close = nullptr;
        return true;
    }
    const size_t from = lower.find_first_not_of(" \t\r\n");
    if (from == std::string::npos) return false;
    const std::string_view rest = std::string_view(lower).substr(from);
    for (const auto& [open, close] : {std::pair{"<style", "</style>"}, std::pair{"<script", "</script>"}}) {
        if (rest.substr(0, std::strlen(open)) != open) continue;
        const size_t n = std::strlen(open);
        if (rest.size() > n && !std::isspace(static_cast<unsigned char>(rest[n])) && rest[n] != '>') continue;  // <styled> is another tag
        if (rest.find(close) == std::string_view::npos) raw_close = close;
        if (stack.size() == 1) hidden_start.push_back(line_of(s.data() + from));
        return true;
    }
    return false;
}

void Builder::html(std::string_view s) {
    size_t i = 0;
    auto text_piece = [&](std::string_view piece) {
        if (piece.find_first_not_of(" \t\r\n") == std::string_view::npos) return;
        std::string t = decode_entities(piece);
        for (char& ch : t)
            if (ch == '\n' || ch == '\r') ch = ' ';
        add_text(t);
    };
    while (i < s.size()) {
        if (html_comment) {
            size_t end = s.find("-->", i);
            if (end == std::string_view::npos) return;  // comment continues in the next chunk
            html_comment = false;
            i = end + 3;
            continue;
        }
        size_t lt = s.find('<', i);
        if (lt == std::string_view::npos) {
            text_piece(s.substr(i));
            return;
        }
        text_piece(s.substr(i, lt - i));
        if (s.substr(lt, 4) == "<!--") {
            html_comment = true;
            i = lt + 4;
            continue;
        }
        size_t gt = s.find('>', lt);
        if (gt == std::string_view::npos) return;
        std::string_view tag = s.substr(lt + 1, gt - lt - 1);
        while (!tag.empty() && (tag.back() == '/' || std::isspace(static_cast<unsigned char>(tag.back())))) tag.remove_suffix(1);
        size_t name_end = 0;
        while (name_end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[name_end]))) name_end++;
        std::string name(tag.substr(0, name_end));
        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (name == "br") {
            add_text("\n");
        } else if (name == "img") {
            std::string src = html_attr(tag, "src");
            if (!src.empty()) {
                std::string w = html_attr(tag, "width");
                float width = w.empty() ? 0.0f : std::strtof(w.c_str(), nullptr);  // "200" or "200px"
                add_image(src, html_attr(tag, "alt"), width);
            }
        }
        i = gt + 1;
    }
}

namespace {

// Is this span just whitespace (and not an image/math/code)?
bool blank_span(const Span& s) {
    return s.image_src.empty() && !s.math && !s.code && s.text.find_first_not_of(" \t\r\n") == std::string::npos;
}

// Post-process paragraphs (recursing into quotes and list items):
// - each $$..$$ span becomes its own Block::Kind::Math (centred, display style);
// - a paragraph holding only pictures becomes a Block::Kind::Images row;
// - whitespace-only leftovers are dropped.
void lift_blocks(std::vector<Block>& blocks) {
    std::vector<Block> out;
    for (Block& b : blocks) {
        lift_blocks(b.children);
        for (auto& item : b.items) lift_blocks(item);
        if (b.kind != Block::Kind::Paragraph) {
            out.push_back(std::move(b));
            continue;
        }
        const size_t first_out = out.size();  // pieces of this paragraph start here
        Block run;  // text collected since the last formula
        auto flush = [&] {
            std::vector<const Span*> content;
            for (const Span& s : run.spans)
                if (!blank_span(s)) content.push_back(&s);
            bool images_only = !content.empty() &&
                               std::all_of(content.begin(), content.end(), [](const Span* s) { return !s->image_src.empty(); });
            if (images_only) {
                Block row;
                row.kind = Block::Kind::Images;
                for (const Span* s : content) {
                    if (s->image_start || row.images.empty()) row.images.push_back({s->image_src, s->text, s->image_width, s->link});
                    else row.images.back().alt += s->text;
                }
                out.push_back(std::move(row));
            } else if (!content.empty()) {
                out.push_back(std::move(run));
            }
            run = Block{};
        };
        for (Span& s : b.spans) {
            if (s.display) {
                flush();
                Block m;
                m.kind = Block::Kind::Math;
                m.text = std::move(s.text);
                out.push_back(std::move(m));
            } else {
                run.spans.push_back(std::move(s));
            }
        }
        flush();
        for (size_t k = first_out; k < out.size(); k++) {  // the pieces keep the paragraph's source lines
            out[k].line_first = b.line_first;
            out[k].line_last = b.line_last;
        }
    }
    blocks = std::move(out);
}

int text(MD_TEXTTYPE type, const MD_CHAR* p, MD_SIZE size, void* user) {
    auto& b = *static_cast<Builder*>(user);
    std::string_view t(p, size);
    if (type == MD_TEXT_HTML && b.hides_raw_html(t)) return 0;  // a <style>/<script> block: not drawn, no block
    b.seen(p);
    if (b.code) {  // inside a fenced/indented code block: keep everything verbatim
        b.code->append(t);
        return 0;
    }
    switch (type) {
        case MD_TEXT_NULLCHAR: break;
        case MD_TEXT_BR: b.add_text("\n"); break;      // hard line break
        case MD_TEXT_SOFTBR: b.add_text(" "); break;   // newline inside a paragraph
        case MD_TEXT_ENTITY: b.add_text(decode_entity(t)); break;
        case MD_TEXT_HTML: b.html(t); break;           // inline or block HTML
        default: b.add_text(t); break;
    }
    return 0;
}

}  // namespace

// --- preprocessing: front matter, footnotes, callouts ---------------------------

std::string strip_front_matter(std::string_view src) {
    std::string_view rest = src;
    if (rest.substr(0, 3) == "\xEF\xBB\xBF") rest.remove_prefix(3);
    auto line_end = [](std::string_view s) { size_t n = s.find('\n'); return n == std::string_view::npos ? s.size() : n; };
    auto trim = [](std::string_view l) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.remove_suffix(1);
        return l;
    };
    size_t n = line_end(rest);
    if (trim(rest.substr(0, n)) != "---" || n >= rest.size()) return std::string(src);
    size_t pos = n + 1;
    while (pos < rest.size()) {
        size_t e = line_end(rest.substr(pos));
        std::string_view l = trim(rest.substr(pos, e));
        pos += e + 1;
        if (l == "---" || l == "...") return std::string(pos >= rest.size() ? std::string_view() : rest.substr(pos));
    }
    return std::string(src);  // no closing line: not front matter
}

std::string callout_kind(std::string_view line) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
    if (line.size() < 4 || line.substr(0, 2) != "[!" || line.back() != ']') return "";
    std::string k(line.substr(2, line.size() - 3));
    for (char& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* ok : {"note", "tip", "important", "warning", "caution"})
        if (k == ok) return k;
    return "";
}

namespace {

// Opening/closing fence of a fenced code block: returns the fence run length (>=3) or 0.
size_t fence_len(std::string_view line, char& ch) {
    size_t i = 0;
    while (i < line.size() && i < 4 && line[i] == ' ') i++;
    if (i >= line.size() || (line[i] != '`' && line[i] != '~')) return 0;
    ch = line[i];
    size_t j = i;
    while (j < line.size() && line[j] == ch) j++;
    return j - i >= 3 ? j - i : 0;
}

// A line that opens a new block (heading, quote, list item, rule), so it can't
// be a lazy continuation of a paragraph.
bool starts_block(std::string_view line) {
    size_t i = line.find_first_not_of(' ');
    if (i == std::string_view::npos || i > 3) return false;
    line.remove_prefix(i);
    const char c = line[0];
    if (c == '#' || c == '>') return true;
    if ((c == '-' || c == '*' || c == '+') && (line.size() == 1 || line[1] == ' ')) return true;  // bullet (or rule)
    if (line.find_first_not_of(c == '_' ? "_ " : "") == std::string_view::npos) return true;       // ___ rule
    size_t d = 0;
    while (d < line.size() && d < 9 && std::isdigit(static_cast<unsigned char>(line[d]))) d++;
    return d > 0 && d < line.size() && (line[d] == '.' || line[d] == ')') && (d + 1 == line.size() || line[d + 1] == ' ');
}

// Footnote definition start `[^label]:` -> label (and the offset of the text), or empty.
std::string def_label(std::string_view line, size_t& text_at) {
    if (line.substr(0, 2) != "[^") return "";
    size_t close = line.find(']', 2);
    if (close == std::string_view::npos || close == 2 || line.substr(close, 2) != "]:") return "";
    std::string_view label = line.substr(2, close - 2);
    if (label.find_first_of(" \t") != std::string_view::npos) return "";
    text_at = close + 2;
    return std::string(label);
}

}  // namespace

std::string extract_footnotes(std::string_view src, std::vector<std::string>& notes, std::vector<int>* line_map) {
    notes.clear();
    if (line_map) line_map->clear();
    // Pass 1: split lines, drop definitions (outside fences), collect label -> text.
    std::vector<std::string> lines;
    std::vector<bool> plain;  // line is outside code fences (refs may be replaced)
    std::vector<int> from;    // input line number of each kept line
    int in_line = -1;         // input line being read
    std::vector<std::pair<std::string, std::string>> defs;
    bool fenced = false;
    char fch = 0;
    size_t flen = 0;
    for (size_t pos = 0; pos <= src.size();) {
        size_t e = src.find('\n', pos);
        if (e == std::string_view::npos) e = src.size();
        std::string_view line = src.substr(pos, e - pos);
        pos = e + 1;
        in_line++;
        std::string_view bare = line;
        if (!bare.empty() && bare.back() == '\r') bare.remove_suffix(1);
        char ch = 0;
        size_t n = fence_len(bare, ch);
        if (fenced) {
            if (n >= flen && ch == fch) fenced = false;
            lines.emplace_back(line);
            from.push_back(in_line);
            plain.push_back(false);
            continue;
        }
        if (n) {
            fenced = true, fch = ch, flen = n;
            lines.emplace_back(line);
            from.push_back(in_line);
            plain.push_back(false);
            continue;
        }
        size_t at = 0;
        std::string label = def_label(bare, at);
        if (!label.empty()) {
            std::string text(bare.substr(at));
            size_t a = text.find_first_not_of(" \t");
            text = a == std::string::npos ? "" : text.substr(a);
            // Continuation lines: indented by >= 2 spaces or a tab, or (as in
            // GFM, "lazy") any text line straight after a text line that
            // doesn't start a new block.
            bool prev_text = !text.empty();
            while (pos < src.size()) {
                size_t e2 = src.find('\n', pos);
                if (e2 == std::string_view::npos) e2 = src.size();
                std::string_view nx = src.substr(pos, e2 - pos);
                if (!nx.empty() && nx.back() == '\r') nx.remove_suffix(1);
                const bool indented = nx.substr(0, 2) == "  " || nx.substr(0, 1) == "\t";
                size_t unused = 0;
                char fc = 0;
                const bool lazy = prev_text && !nx.empty() && nx.find_first_not_of(" \t") != std::string_view::npos &&
                                  !starts_block(nx) && !fence_len(nx, fc) && def_label(nx, unused).empty();
                if (!indented && !lazy) break;
                prev_text = nx.find_first_not_of(" \t") != std::string_view::npos;
                size_t b = nx.find_first_not_of(" \t");
                text += "\n";
                if (b != std::string_view::npos) text += nx.substr(b);
                pos = e2 + 1;
                in_line++;
            }
            defs.emplace_back(std::move(label), std::move(text));
            continue;
        }
        lines.emplace_back(line);
        from.push_back(in_line);
        plain.push_back(true);
    }
    if (defs.empty()) {
        if (line_map)
            for (size_t i = 0; i < lines.size(); i++) line_map->push_back(int(i));
        return std::string(src);
    }
    if (line_map) *line_map = from;
    // Pass 2: replace references, numbering by first reference.
    std::vector<std::string> order;  // labels by number
    std::string out;
    for (size_t li = 0; li < lines.size(); li++) {
        const std::string& l = lines[li];
        if (li) out += '\n';
        if (!plain[li]) {
            out += l;
            continue;
        }
        for (size_t i = 0; i < l.size();) {
            if (l[i] == '`') {  // skip a code span (matching backtick run)
                size_t j = i;
                while (j < l.size() && l[j] == '`') j++;
                std::string run = l.substr(i, j - i);
                size_t close = l.find(run, j);
                size_t end = close == std::string::npos ? j : close + run.size();
                out.append(l, i, end - i);
                i = end;
                continue;
            }
            if (l[i] == '[' && i + 1 < l.size() && l[i + 1] == '^' && (i == 0 || l[i - 1] != '\\')) {
                size_t close = l.find(']', i + 2);
                if (close != std::string::npos && close > i + 2) {
                    std::string label = l.substr(i + 2, close - i - 2);
                    auto d = std::find_if(defs.begin(), defs.end(), [&](const auto& p) { return p.first == label; });
                    if (d != defs.end() && label.find_first_of(" \t") == std::string::npos) {
                        size_t n = size_t(std::find(order.begin(), order.end(), label) - order.begin());
                        if (n == order.size()) {
                            order.push_back(label);
                            notes.push_back(d->second);
                        }
                        std::string num = std::to_string(n + 1);
                        out += "[\\[" + num + "\\]](#fn-" + num + ")";
                        i = close + 1;
                        continue;
                    }
                }
            }
            out += l[i++];
        }
    }
    return out;
}

namespace {

// Plain text of the first line of `spans` (up to the first soft/hard break);
// `used` = how many spans that covers (including the break).
std::string first_line(const Spans& spans, size_t& used) {
    std::string t;
    used = 0;
    for (const Span& s : spans) {
        if (s.code || s.math || !s.image_src.empty() || !s.link.empty() || s.bold || s.italic || s.strike) return "";
        used++;
        if (s.text == " " || s.text == "\n") return t;
        t += s.text;
    }
    return t;
}

// md4c doesn't let a table interrupt a paragraph, so the rows after
// `> [!CAUTION]` would join the marker's paragraph as text. An empty quote
// line after each marker line makes the marker a paragraph of its own
// (detect_callouts then drops it).
std::string split_callout_markers(std::string_view src, std::vector<int>* line_map = nullptr) {
    std::string out;
    int in_line = -1;
    if (line_map) line_map->clear();
    bool fenced = false;
    char fch = 0;
    size_t flen = 0;
    size_t prev_depth = 0;  // quote depth of the previous line, if it had text
    for (size_t pos = 0; pos <= src.size();) {
        size_t e = src.find('\n', pos);
        if (e == std::string_view::npos) e = src.size();
        std::string_view line = src.substr(pos, e - pos);
        if (pos) out += '\n';
        out += line;
        pos = e + 1;
        in_line++;
        if (line_map) line_map->push_back(in_line);
        // Peel `>` markers (each with up to 3 leading spaces and one optional space after).
        std::string_view rest = line;
        if (!rest.empty() && rest.back() == '\r') rest.remove_suffix(1);
        size_t depth = 0;
        for (;;) {
            size_t i = 0;
            while (i < rest.size() && i < 3 && rest[i] == ' ') i++;
            if (i >= rest.size() || rest[i] != '>') break;
            rest.remove_prefix(i + 1);
            if (!rest.empty() && rest[0] == ' ') rest.remove_prefix(1);
            depth++;
        }
        char ch = 0;
        size_t n = fence_len(rest, ch);
        if (fenced) {
            if (n >= flen && ch == fch) fenced = false;
            prev_depth = 0;
            continue;
        }
        if (n) {
            fenced = true, fch = ch, flen = n;
            prev_depth = 0;
            continue;
        }
        // Only a marker that opens its quote (not a lazy line of a paragraph).
        if (depth && depth > prev_depth && !callout_kind(rest).empty()) {
            out += '\n';
            for (size_t d = 0; d < depth; d++) out += '>';
            if (line_map) line_map->push_back(in_line);
        }
        prev_depth = rest.find_first_not_of(" \t\r") == std::string_view::npos ? 0 : depth;
    }
    return out;
}

// Quotes that start with a [!KIND] line become callouts (recursing everywhere).
void detect_callouts(std::vector<Block>& blocks) {
    for (Block& b : blocks) {
        detect_callouts(b.children);
        for (auto& item : b.items) detect_callouts(item);
        if (b.kind != Block::Kind::Quote || b.children.empty() || b.children[0].kind != Block::Kind::Paragraph) continue;
        Block& p = b.children[0];
        size_t used = 0;
        std::string kind = callout_kind(first_line(p.spans, used));
        if (kind.empty()) continue;
        b.callout = kind;
        b.title_line = p.line_first;
        p.spans.erase(p.spans.begin(), p.spans.begin() + static_cast<std::ptrdiff_t>(used));
        if (std::all_of(p.spans.begin(), p.spans.end(), blank_span)) b.children.erase(b.children.begin());
    }
}

MD_PARSER make_parser() {
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = enter_block;
    parser.leave_block = leave_block;
    parser.enter_span = enter_span;
    parser.leave_span = leave_span;
    parser.text = text;
    return parser;
}

}  // namespace

namespace {

// A thematic break line (`---`, `* * *`, `___`).
bool is_rule_line(std::string_view l) {
    size_t i = l.find_first_not_of(' ');
    if (i == std::string_view::npos) return false;
    const char c = l[i];
    if (c != '-' && c != '*' && c != '_') return false;
    size_t n = 0;
    for (char ch : l.substr(i)) {
        if (ch == c) n++;
        else if (ch != ' ' && ch != '\t' && ch != '\r') return false;
    }
    return n >= 3;
}

// Does `line` open/close a code fence once any quote markers, list markers and indentation are
// peeled off its front (`> ```rust`, `- ```` ...)?
bool fence_in_container(std::string_view line) {
    for (;;) {
        size_t i = 0;
        while (i < line.size() && line[i] == ' ') i++;
        line.remove_prefix(i);
        if (line.empty()) return false;
        if (line[0] == '>') {
            line.remove_prefix(1);
            continue;
        }
        if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line.size() > 1 && line[1] == ' ') {
            line.remove_prefix(2);
            continue;
        }
        size_t d = 0;
        while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) d++;
        if (d && d + 1 < line.size() && (line[d] == '.' || line[d] == ')') && line[d + 1] == ' ') {
            line.remove_prefix(d + 2);
            continue;
        }
        break;
    }
    char ch = 0;
    return fence_len(line, ch) > 0;
}

// Only `>` markers and blanks (an empty line inside a quote).
bool blank_in_quote(std::string_view line) {
    return line.find_first_not_of(" \t\r>") == std::string_view::npos;
}

// Give each top-level block its source lines (in the original text, see Block::line_first).
// Block starts come from md4c's text pointers (Builder::top_start); a block's end is the
// line before the next block's start, minus blank lines. `lines` below are 0-based
// indices into `src` (the text md4c parsed); the two maps and `front_lines` lead back to the input.
void assign_lines(Builder& b, const std::string& src, const std::vector<int>& map_callouts, const std::vector<int>& map_notes,
                  int front_lines) {
    const size_t n = b.doc.size();
    b.top_start.resize(n, -1);
    const size_t nlines = b.line_starts.size();
    auto line_text = [&](size_t i) {
        const size_t from = b.line_starts[i];
        const size_t to = i + 1 < nlines ? b.line_starts[i + 1] - 1 : src.size();
        return std::string_view(src).substr(from, to - from);
    };
    auto blank = [&](size_t i) { return line_text(i).find_first_not_of(" \t\r") == std::string_view::npos; };
    std::vector<int> start(n, 0), end(n, 0);
    // Starts: a code block's text begins after its fence line; a block without
    // any pointer (a rule) is the first rule-looking line after the previous block's start.
    for (size_t i = 0; i < n; i++) {
        int s = b.top_start[i];
        if (s >= 0 && i < b.top_is_code.size() && b.top_is_code[i] && s > 0) {
            char ch = 0;
            if (!fence_len(line_text(size_t(s)), ch) && fence_len(line_text(size_t(s - 1)), ch)) s--;  // (an info string is on the fence line already)
        }
        if (s > 0 && i < b.sub_code.size() && !b.sub_code[i].empty() && b.sub_code[i][0] &&  // a list / quote opening with a bare fence
            !fence_in_container(line_text(size_t(s))) && fence_in_container(line_text(size_t(s - 1))))
            s--;
        if (s > 0 && b.doc[i].kind == Block::Kind::Paragraph) {  // `$$` alone on the line above: the formula starts there
            std::string_view up = line_text(size_t(s - 1));
            while (!up.empty() && (up.back() == ' ' || up.back() == '\t' || up.back() == '\r')) up.remove_suffix(1);
            while (!up.empty() && up.front() == ' ') up.remove_prefix(1);
            if (up == "$$") s--;
        }
        if (s < 0) {  // no text at all: a rule or an empty code block
            const int from = i > 0 ? start[i - 1] + 1 : 0;
            s = from;
            char ch = 0;
            const bool code = b.doc[i].kind == Block::Kind::Code;
            while (size_t(s) < nlines && !(code ? fence_len(line_text(size_t(s)), ch) > 0 : is_rule_line(line_text(size_t(s))))) s++;
            if (size_t(s) >= nlines) s = std::min(from, int(nlines) - 1);
        }
        start[i] = std::max(s, i > 0 ? start[i - 1] : 0);
    }
    for (size_t i = 0; i < n; i++) {
        int next = i + 1 < n ? std::max(start[i + 1], start[i] + 1) : int(nlines);
        for (int h : b.hidden_start)  // a hidden <style>/<script> block belongs to no block
            if (h > start[i] && h < next) next = h;
        int e = next - 1;
        e = std::min(e, int(nlines) - 1);
        while (e > start[i] && blank(size_t(e))) e--;
        end[i] = std::max(e, start[i]);
    }
    auto original = [&](int l) {  // 0-based line of `src` -> 1-based line of the input
        l = std::clamp(l, 0, int(map_callouts.size()) - 1);
        int m = map_callouts.empty() ? l : map_callouts[size_t(l)];
        m = map_notes.empty() ? m : map_notes[size_t(std::clamp(m, 0, int(map_notes.size()) - 1))];
        return m + front_lines + 1;
    };
    for (size_t i = 0; i < n; i++) {
        b.doc[i].line_first = original(start[i]);
        b.doc[i].line_last = original(end[i]);
    }
    // One level down (spec 18): the items of a top-level list and the children of a top-level
    // quote get their own ranges, by the same rules (start = first pointer, end = before the next).
    for (size_t i = 0; i < n && i < b.sub_start.size(); i++) {
        Block& blk = b.doc[i];
        const bool is_list = blk.kind == Block::Kind::List, is_quote = blk.kind == Block::Kind::Quote;
        const std::vector<int>& first = b.sub_start[i];
        if (!(is_list || is_quote) || first.empty()) continue;
        const size_t m = first.size();
        std::vector<int> st(m), en(m);
        for (size_t k = 0; k < m; k++) {
            int s = first[k];
            if (s > 0 && b.sub_code[i][k] && !fence_in_container(line_text(size_t(s))) && fence_in_container(line_text(size_t(s - 1)))) s--;
            const int lo = k > 0 ? st[k - 1] : start[i];
            if (s < 0) s = std::min(lo + (k > 0 ? 1 : 0), int(nlines) - 1);
            st[k] = std::max(s, lo);
        }
        for (size_t k = 0; k < m; k++) {
            int e = (k + 1 < m ? std::max(st[k + 1], st[k] + 1) : end[i] + 1) - 1;
            e = std::min(e, std::max(end[i], st[k]));
            while (e > st[k] && blank_in_quote(line_text(size_t(e)))) e--;
            en[k] = std::max(e, st[k]);
        }
        for (size_t k = 0; k < m; k++) {
            if (is_list) blk.item_lines.push_back({original(st[k]), original(en[k])});
            else if (k < blk.children.size()) blk.children[k].line_first = original(st[k]), blk.children[k].line_last = original(en[k]);
        }
    }
}

}  // namespace

void lift_display_blocks(std::vector<Block>& blocks) { lift_blocks(blocks); }

Document parse(std::string_view src_in) {
    std::vector<std::string> notes;
    std::vector<int> map_notes, map_callouts;
    const std::string stripped = strip_front_matter(src_in);
    const int front_lines = int(std::count(src_in.begin(), src_in.begin() + std::ptrdiff_t(src_in.size() - stripped.size()), '\n'));
    const std::string src = split_callout_markers(extract_footnotes(stripped, notes, &map_notes), &map_callouts);
    const MD_PARSER parser = make_parser();
    Builder b;
    b.base = src.data();
    b.base_size = src.size();
    b.line_starts.push_back(0);
    for (size_t i = 0; i < src.size(); i++)
        if (src[i] == '\n') b.line_starts.push_back(i + 1);
    md_parse(src.data(), static_cast<MD_SIZE>(src.size()), &parser, &b);
    assign_lines(b, src, map_callouts, map_notes, front_lines);
    lift_blocks(b.doc);
    detect_callouts(b.doc);
    // A paragraph that is only `. . .` is a pause (pandoc's syntax, spec 26): not shown.
    std::erase_if(b.doc, [](const Block& blk) {
        if (blk.kind != Block::Kind::Paragraph) return false;
        std::string text;
        for (const Span& s : blk.spans) text += s.text;
        return text == ". . .";
    });
    if (!notes.empty()) {
        // The footnote section has no place of its own in the source: it takes the last line.
        const int tail_line = b.doc.empty() ? 1 : b.doc.back().line_last;
        Block rule;
        rule.kind = Block::Kind::Rule;
        rule.line_first = rule.line_last = tail_line;
        b.doc.push_back(std::move(rule));
        for (size_t i = 0; i < notes.size(); i++) {
            // Footnote text is inline Markdown (no nested preprocessing).
            Builder nb;
            md_parse(notes[i].data(), static_cast<MD_SIZE>(notes[i].size()), &parser, &nb);
            lift_blocks(nb.doc);
            Block para;
            para.kind = Block::Kind::Paragraph;
            para.footnote = static_cast<int>(i + 1);
            para.line_first = para.line_last = tail_line;
            Span num;
            num.text = std::to_string(i + 1) + ". ";
            para.spans.push_back(std::move(num));
            size_t k = 0;
            if (!nb.doc.empty() && nb.doc[0].kind == Block::Kind::Paragraph) {
                for (Span& s : nb.doc[0].spans) para.spans.push_back(std::move(s));
                k = 1;
            }
            Span back;  // " ↩": back to the first reference (anchor fnref-n, see main.cpp)
            back.text = " ↩";  // " ↩"
            back.link = "#fnref-" + std::to_string(i + 1);
            para.spans.push_back(std::move(back));
            b.doc.push_back(std::move(para));
            for (; k < nb.doc.size(); k++) {
                nb.doc[k].line_first = nb.doc[k].line_last = tail_line;
                b.doc.push_back(std::move(nb.doc[k]));
            }
        }
    }
    return std::move(b.doc);
}

const char* looks_binary(std::string_view bytes) {
    const std::string_view sample = bytes.substr(0, 8192);
    struct Signature {
        std::string_view start;
        const char* name;
    };
    static const Signature signatures[] = {
        {"%PDF", "a PDF"}, {"\x89PNG", "a PNG image"}, {"\xFF\xD8\xFF", "a JPEG image"},
        {"GIF8", "a GIF image"}, {std::string_view("PK\x03\x04", 4), "a ZIP archive"},
    };
    for (const Signature& s : signatures)
        if (sample.starts_with(s.start)) return s.name;

    // Invalid UTF-8: count the bytes that don't belong to a well-formed character (one cut off by
    // the end of the sample is not counted).
    size_t bad = 0;
    for (size_t i = 0; i < sample.size();) {
        const unsigned char c = static_cast<unsigned char>(sample[i]);
        const size_t need = c < 0x80 ? 0 : c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : SIZE_MAX;
        size_t ok = 0;
        if (need != SIZE_MAX)
            while (ok < need && i + 1 + ok < sample.size() && (static_cast<unsigned char>(sample[i + 1 + ok]) & 0xC0) == 0x80) ok++;
        if (need != SIZE_MAX && ok == need) {
            i += 1 + need;
        } else if (need != SIZE_MAX && i + 1 + ok >= sample.size()) {
            break;  // cut off by the end of the sample
        } else {
            bad++;
            i++;
        }
    }
    const bool nul = sample.find('\0') != std::string_view::npos;
    if (!nul && bad * 10 <= sample.size() * 3) return nullptr;
    // ("MZ" only names a file the rules above caught: two letters could start a text file.)
    return sample.starts_with("MZ") ? "a Windows program" : "binary data";
}

std::string binary_message(const std::string& name, const std::string& kind) {
    return name + " is not a text file (looks like " + kind + "). Calligrus shows Markdown and Org files.";
}

std::string too_large_message(const std::string& name, uint64_t bytes, bool android) {
    auto mb = [](double b) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f MB", b / 1e6);
        return std::string(buf);
    };
    if (!android) return name + " is " + mb(double(bytes)) + " and may take a long time and a lot of memory to display.";
    return name + " is too large to display: " + mb(double(bytes)) + ", the limit is " + mb(double(kMaxFileBytesAndroid)) + ".";
}

bool is_plain_text_path(std::string_view path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string ext(path.substr(dot));
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".csv" || ext == ".tsv" || ext == ".log";
}

std::string plain_text_as_markdown(std::string_view text) {
    size_t longest = 0, run = 0;
    for (char c : text) {
        run = c == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const std::string fence(std::max<size_t>(3, longest + 1), '`');
    std::string out = fence + "\n";
    out.append(text);
    if (text.empty() || text.back() != '\n') out += '\n';
    out += fence + "\n";
    return out;
}

}  // namespace md
