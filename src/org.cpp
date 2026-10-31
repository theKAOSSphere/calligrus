// Org-mode parser (spec 25), line based. See org.h.
#include "org.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace md {
namespace {

using K = Block::Kind;

// ---- small string helpers ----------------------------------------------------------------

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// Does `s` start with `prefix`, ignoring case?
bool starts_ci(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && lower(s.substr(0, prefix.size())) == lower(prefix);
}

size_t indent_of(std::string_view s) {
    size_t n = 0;
    while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) n++;
    return n;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// ---- parser state ------------------------------------------------------------------------

struct Ctx {
    std::vector<std::string> lines;                 // the source, one entry per line (no \r)
    std::map<std::string, std::string> fn_defs;     // footnote label -> text
    std::vector<std::string> fn_order;              // labels in order of first reference
};

int footnote_number(Ctx& c, const std::string& label) {
    for (size_t i = 0; i < c.fn_order.size(); i++)
        if (c.fn_order[i] == label) return int(i + 1);
    c.fn_order.push_back(label);
    return int(c.fn_order.size());
}

// ---- inline markup -----------------------------------------------------------------------

struct Style {
    bool bold = false, italic = false, strike = false;
    std::string link;
};

// May an emphasis marker open before position `i` / close after position `j` of `s`?
bool can_open(std::string_view s, size_t i) {
    if (i + 1 >= s.size() || is_space(s[i + 1])) return false;
    if (i == 0) return true;
    const char p = s[i - 1];
    return is_space(p) || std::string_view("-('\"{").find(p) != std::string_view::npos;
}
bool can_close(std::string_view s, size_t j) {
    if (is_space(s[j - 1])) return false;
    if (j + 1 >= s.size()) return true;
    const char n = s[j + 1];
    return is_space(n) || std::string_view("-.,;:!?')}\"\\").find(n) != std::string_view::npos;
}

bool is_image_target(std::string_view t) {
    const size_t dot = t.rfind('.');
    if (dot == std::string_view::npos) return false;
    const std::string ext = lower(t.substr(dot + 1));
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "svg" || ext == "webp" || ext == "bmp";
}

void parse_inline(std::string_view s, const Style& st, Ctx& ctx, Spans& out);

void push_text(Spans& out, std::string& buf, const Style& st) {
    if (buf.empty()) return;
    Span sp;
    sp.text = std::move(buf);
    sp.bold = st.bold, sp.italic = st.italic, sp.strike = st.strike;
    sp.link = st.link;
    out.push_back(std::move(sp));
    buf.clear();
}

// Inline markup of one piece of text (one paragraph line, a headline, a table cell).
void parse_inline(std::string_view s, const Style& st, Ctx& ctx, Spans& out) {
    std::string buf;
    size_t i = 0;
    const size_t n = s.size();
    auto special = [&](Span sp) {
        push_text(out, buf, st);
        sp.bold = sp.bold || st.bold, sp.italic = sp.italic || st.italic, sp.strike = sp.strike || st.strike;
        if (sp.link.empty()) sp.link = st.link;
        out.push_back(std::move(sp));
    };
    auto math = [&](std::string tex, bool display) {
        Span sp;
        sp.math = true;
        sp.display = display;
        sp.text = std::move(tex);
        special(std::move(sp));
    };
    while (i < n) {
        const char c = s[i];
        const bool at_boundary = i == 0 || is_space(s[i - 1]) || std::string_view("-('\"{").find(s[i - 1]) != std::string_view::npos;
        // Links: [[target][desc]] / [[target]]; footnote references [fn:name].
        if (c == '[' && i + 1 < n && s[i + 1] == '[') {
            const size_t end = s.find("]]", i + 2);
            if (end != std::string_view::npos) {
                std::string_view body = s.substr(i + 2, end - i - 2);
                std::string_view target = body, desc;
                bool has_desc = false;
                if (const size_t m = body.find("]["); m != std::string_view::npos) {
                    target = body.substr(0, m), desc = body.substr(m + 2), has_desc = true;
                }
                std::string t(target);
                if (t.rfind("file:", 0) == 0) t.erase(0, 5);
                if (t.rfind("*", 0) == 0) t = "#" + t.substr(1);  // [[*Heading text]] -> that heading's slug
                if (!has_desc && is_image_target(t)) {
                    Span sp;
                    sp.image_src = t;
                    sp.image_start = true;
                    sp.text = t.substr(t.find_last_of("/\\") == std::string::npos ? 0 : t.find_last_of("/\\") + 1);
                    special(std::move(sp));
                } else {
                    Style ls = st;
                    ls.link = t;
                    if (has_desc) {
                        push_text(out, buf, st);
                        parse_inline(desc, ls, ctx, out);
                    } else {
                        Span sp;
                        sp.text = (!t.empty() && t[0] == '#') ? t.substr(1) : t;
                        sp.link = t;
                        special(std::move(sp));
                    }
                }
                i = end + 2;
                continue;
            }
        }
        if (c == '[' && s.substr(i, 4) == "[fn:") {
            const size_t end = s.find(']', i);
            if (end != std::string_view::npos) {
                const std::string label(s.substr(i + 4, end - i - 4));
                if (ctx.fn_defs.count(label) && label.find_first_of(" \t") == std::string::npos) {
                    Span sp;
                    sp.text = "[" + std::to_string(footnote_number(ctx, label)) + "]";
                    sp.link = "#fn-" + std::to_string(footnote_number(ctx, label));
                    sp.small = true;
                    special(std::move(sp));
                    i = end + 1;
                    continue;
                }
            }
        }
        // Bare URLs.
        if (at_boundary && (s.substr(i, 7) == "http://" || s.substr(i, 8) == "https://")) {
            size_t e = i;
            while (e < n && !is_space(s[e]) && s[e] != '<' && s[e] != '>') e++;
            while (e > i && std::string_view(".,;:!?)'\"").find(s[e - 1]) != std::string_view::npos) e--;
            Span sp;
            sp.text = std::string(s.substr(i, e - i));
            sp.link = sp.text;
            special(std::move(sp));
            i = e;
            continue;
        }
        // Math.
        if (c == '\\' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '[')) {
            const bool disp = s[i + 1] == '[';
            const size_t end = s.find(disp ? "\\]" : "\\)", i + 2);
            if (end != std::string_view::npos) {
                math(std::string(trim(s.substr(i + 2, end - i - 2))), disp);
                i = end + 2;
                continue;
            }
        }
        if (c == '$') {
            if (i + 1 < n && s[i + 1] == '$') {
                const size_t end = s.find("$$", i + 2);
                if (end != std::string_view::npos && end > i + 2) {
                    math(std::string(trim(s.substr(i + 2, end - i - 2))), true);
                    i = end + 2;
                    continue;
                }
            } else if (i + 1 < n && !is_space(s[i + 1])) {
                for (size_t j = i + 1; j < n; j++) {
                    if (s[j] == '$' && !is_space(s[j - 1]) && !(j + 1 < n && is_digit(s[j + 1]))) {
                        math(std::string(s.substr(i + 1, j - i - 1)), false);
                        i = j + 1;
                        goto next;
                    }
                }
            }
        }
        // Emphasis: *bold* /italic/ +strike+ =verbatim= ~code~ _underline_
        if (std::string_view("*/+=~_").find(c) != std::string_view::npos && can_open(s, i)) {
            for (size_t j = i + 2; j < n; j++) {
                if (s[j] != c || !can_close(s, j)) continue;
                const std::string_view body = s.substr(i + 1, j - i - 1);
                if (c == '=' || c == '~') {
                    Span sp;
                    sp.text = std::string(body);
                    sp.code = true;
                    special(std::move(sp));
                } else {
                    push_text(out, buf, st);
                    Style inner = st;
                    if (c == '*') inner.bold = true;
                    if (c == '/') inner.italic = true;
                    if (c == '+') inner.strike = true;
                    parse_inline(body, inner, ctx, out);
                }
                i = j + 1;
                goto next;
            }
        }
        buf += c;
        i++;
    next:;
    }
    push_text(out, buf, st);
}

Spans inline_spans(std::string_view s, Ctx& ctx) {
    Spans out;
    parse_inline(s, Style{}, ctx, out);
    return out;
}

// ---- line classification -----------------------------------------------------------------

bool blank_line(const std::string& l) { return trim(l).empty(); }

// A headline: 1+ stars at column 0, then a space.
int headline_level(std::string_view l) {
    size_t n = 0;
    while (n < l.size() && l[n] == '*') n++;
    return n > 0 && n < l.size() && l[n] == ' ' ? int(n) : 0;
}

struct Bullet {
    size_t indent = 0;     // columns before the marker
    size_t text_at = 0;    // offset of the item text in the line
    bool ordered = false;
    unsigned number = 1;
};

// `- x`, `+ x`, indented `* x`, `1. x`, `1) x`.
bool parse_bullet(std::string_view l, Bullet& b) {
    const size_t ind = indent_of(l);
    if (ind >= l.size()) return false;
    size_t k = ind;
    if (l[k] == '-' || l[k] == '+' || (l[k] == '*' && ind > 0)) {
        k++;
        b.ordered = false;
    } else if (is_digit(l[k])) {
        size_t d = k;
        while (d < l.size() && is_digit(l[d])) d++;
        if (d >= l.size() || (l[d] != '.' && l[d] != ')') || d - k > 9) return false;
        b.number = unsigned(std::stoul(std::string(l.substr(k, d - k))));
        b.ordered = true;
        k = d + 1;
    } else {
        return false;
    }
    if (k < l.size() && l[k] != ' ' && l[k] != '\t') return false;
    b.indent = ind;
    while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
    b.text_at = k;
    return true;
}

bool is_rule(std::string_view l) {
    l = trim(l);
    return l.size() >= 5 && l.find_first_not_of('-') == std::string_view::npos;
}

bool is_table_line(std::string_view l) { return !trim(l).empty() && trim(l)[0] == '|'; }

bool is_fixed_width(std::string_view l) {
    l = l.substr(indent_of(l));
    return !l.empty() && l[0] == ':' && (l.size() == 1 || l[1] == ' ');
}

bool is_comment(std::string_view l) {
    l = l.substr(indent_of(l));
    return !l.empty() && l[0] == '#' && (l.size() == 1 || l[1] == ' ');
}

// `[fn:name] text` at column 0: the label, and where the text starts.
bool footnote_def(std::string_view l, std::string& label, size_t& text_at) {
    if (l.substr(0, 4) != "[fn:") return false;
    const size_t e = l.find(']');
    if (e == std::string_view::npos || e == 4) return false;
    label = std::string(l.substr(4, e - 4));
    if (label.find_first_of(" \t") != std::string::npos) return false;
    text_at = e + 1;
    return true;
}

// `:NAME:` (a drawer start) -- not `:END:`.
bool drawer_start(std::string_view l) {
    l = trim(l);
    if (l.size() < 3 || l.front() != ':' || l.back() != ':') return false;
    for (size_t i = 1; i + 1 < l.size(); i++)
        if (!std::isalnum(static_cast<unsigned char>(l[i])) && l[i] != '_' && l[i] != '-') return false;
    return lower(l) != ":end:";
}

bool planning_line(std::string_view l) {
    l = trim(l);
    return starts_ci(l, "SCHEDULED:") || starts_ci(l, "DEADLINE:") || starts_ci(l, "CLOSED:");
}

bool math_start(std::string_view l) {
    l = trim(l);
    return l.substr(0, 2) == "\\[" || l.substr(0, 2) == "$$" || starts_ci(l, "\\begin{");
}

// Does this line begin something other than paragraph text?
bool starts_element(const std::string& line) {
    std::string_view l = line;
    Bullet b;
    std::string label;
    size_t at;
    return headline_level(l) || (l.substr(0, 2) == "#+") || is_comment(l) || parse_bullet(l, b) || is_table_line(l) ||
           is_fixed_width(l) || footnote_def(l, label, at) || is_rule(l) || math_start(l);
}

// "#+BEGIN_SRC rust" -> name "src", args "rust". False if not a begin line.
bool begin_line(std::string_view l, std::string& name, std::string& args) {
    l = trim(l);
    if (!starts_ci(l, "#+begin_")) return false;
    l.remove_prefix(8);
    size_t e = 0;
    while (e < l.size() && !is_space(l[e])) e++;
    name = lower(l.substr(0, e));
    args = std::string(trim(l.substr(e)));
    return true;
}

// ---- blocks ------------------------------------------------------------------------------

std::vector<Block> parse_blocks(Ctx& c, size_t from, size_t to);

bool is_table_rule(std::string_view l) {
    l = trim(l);
    return l.size() >= 2 && l[0] == '|' && l[1] == '-';
}

std::vector<std::string> split_cells(std::string_view l) {
    l = trim(l);
    if (!l.empty() && l.front() == '|') l.remove_prefix(1);
    if (!l.empty() && l.back() == '|') l.remove_suffix(1);
    std::vector<std::string> cells;
    size_t p = 0;
    for (;;) {
        const size_t e = l.find('|', p);
        cells.emplace_back(trim(l.substr(p, e == std::string_view::npos ? std::string_view::npos : e - p)));
        if (e == std::string_view::npos) break;
        p = e + 1;
    }
    return cells;
}

// A cookie cell: `<l>` `<c>` `<r>` `<N>` `<lN>`...; returns alignment letter ('l','c','r', or 'n' for width only).
char cookie(const std::string& cell) {
    if (cell.size() < 3 || cell.front() != '<' || cell.back() != '>') return 0;
    size_t i = 1;
    char kind = 'n';
    if (cell[i] == 'l' || cell[i] == 'c' || cell[i] == 'r') kind = cell[i++];
    for (; i + 1 < cell.size(); i++)
        if (!is_digit(cell[i])) return 0;
    return kind;
}

Block make_table(Ctx& c, size_t from, size_t to) {  // lines [from, to)
    Block t;
    t.kind = K::Table;
    t.line_first = int(from) + 1;
    t.line_last = int(to);
    std::vector<std::vector<std::string>> rows;
    size_t first_rule = std::string::npos;  // number of data rows above the first rule line
    for (size_t i = from; i < to; i++) {
        if (is_table_rule(c.lines[i])) {
            if (first_rule == std::string::npos) first_rule = rows.size();
            continue;
        }
        std::vector<std::string> cells = split_cells(c.lines[i]);
        const bool any_cookie = std::any_of(cells.begin(), cells.end(), [](const std::string& s) { return cookie(s) != 0; });
        const bool only_cookies = std::all_of(cells.begin(), cells.end(), [](const std::string& s) { return s.empty() || cookie(s) != 0; });
        if (any_cookie && only_cookies) {  // alignment row: hidden
            for (size_t k = 0; k < cells.size(); k++) {
                const char a = cookie(cells[k]);
                if (a == 0 || a == 'n') continue;
                if (t.aligns.size() <= k) t.aligns.resize(k + 1, Align::Default);
                if (t.aligns[k] == Align::Default) t.aligns[k] = a == 'l' ? Align::Left : a == 'c' ? Align::Center : Align::Right;
            }
            continue;
        }
        rows.push_back(std::move(cells));
    }
    size_t body_from = 0;
    if (first_rule != std::string::npos && first_rule > 0 && !rows.empty()) {  // rows above the first rule: header
        for (const std::string& cell : rows[0]) t.header.push_back(inline_spans(cell, c));
        body_from = 1;
    }
    for (size_t r = body_from; r < rows.size(); r++) {
        std::vector<Spans> row;
        for (const std::string& cell : rows[r]) row.push_back(inline_spans(cell, c));
        t.rows.push_back(std::move(row));
    }
    return t;
}

Block make_paragraph(Spans spans, int first, int last) {
    Block p;
    p.kind = K::Paragraph;
    p.spans = std::move(spans);
    p.line_first = first;
    p.line_last = last;
    return p;
}

// Append the text of one paragraph line: a `\` at the end is a hard break.
void add_line(Spans& spans, std::string_view line, bool first, Ctx& c) {
    std::string_view t = trim(line);
    bool hard = false;
    if (!t.empty() && t.back() == '\\') hard = true, t.remove_suffix(1);
    if (!first && !spans.empty() && spans.back().text != "\n") {
        Span sep;
        sep.text = " ";
        spans.push_back(std::move(sep));
    }
    Spans line_spans = inline_spans(t, c);
    for (Span& s : line_spans) spans.push_back(std::move(s));
    if (hard) {
        Span br;
        br.text = "\n";
        spans.push_back(std::move(br));
    }
}

// One list: lines [i, to) start with a bullet at some indentation. Returns the end line (exclusive).
size_t parse_list(Ctx& c, size_t i, size_t to, Block& list) {
    Bullet first;
    parse_bullet(c.lines[i], first);
    list.kind = K::List;
    list.ordered = first.ordered;
    list.start = first.number;
    list.line_first = int(i) + 1;
    size_t last_used = i;
    while (i < to) {
        Bullet b;
        if (!parse_bullet(c.lines[i], b) || b.indent != first.indent || b.ordered != first.ordered) break;
        // The item runs over blank lines and more-indented lines.
        size_t e = i + 1;
        size_t used = i;
        while (e < to) {
            if (blank_line(c.lines[e])) {
                e++;
                continue;
            }
            if (indent_of(c.lines[e]) <= first.indent) break;
            used = e;
            e++;
        }
        // `used` = last non-blank line of the item; `e` may be past trailing blanks.
        std::string_view text = std::string_view(c.lines[i]).substr(b.text_at);
        Spans spans;
        std::string_view rest = text;
        const char* box = nullptr;
        if (rest.size() >= 3 && rest[0] == '[' && rest[2] == ']' && (rest.size() == 3 || rest[3] == ' ')) {
            if (rest[1] == ' ' || rest[1] == '-') box = "\xE2\x98\x90 ";  // ☐ ([-] counts as open)
            if (rest[1] == 'X' || rest[1] == 'x') box = "\xE2\x98\x91 ";  // ☑
            if (box) rest = trim(rest.substr(3));
        }
        if (box) {
            Span m;
            m.text = box;
            spans.push_back(std::move(m));
        }
        const size_t dd = rest.find(" :: ");
        if (dd != std::string_view::npos && !box) {  // description item: term (bold), " — ", text
            Spans term = inline_spans(trim(rest.substr(0, dd)), c);
            for (Span& s : term) s.bold = true, spans.push_back(std::move(s));
            Span dash;
            dash.text = " \xE2\x80\x94 ";
            spans.push_back(std::move(dash));
            rest = rest.substr(dd + 4);
        }
        {
            std::string_view t = trim(rest);
            bool hard = !t.empty() && t.back() == '\\';
            if (hard) t.remove_suffix(1);
            Spans body = inline_spans(t, c);
            for (Span& s : body) spans.push_back(std::move(s));
            if (hard) spans.push_back(Span{"\n"});
        }
        // Continuation lines of the paragraph, then nested content.
        size_t j = i + 1;
        while (j <= used && !blank_line(c.lines[j]) && !starts_element(c.lines[j])) {
            add_line(spans, c.lines[j], false, c);
            j++;
        }
        std::vector<Block> item;
        item.push_back(make_paragraph(std::move(spans), int(i) + 1, int(j)));
        if (j <= used) {
            std::vector<Block> nested = parse_blocks(c, j, used + 1);
            for (Block& nb : nested) item.push_back(std::move(nb));
        }
        list.items.push_back(std::move(item));
        list.item_lines.push_back({int(i) + 1, int(used) + 1});
        last_used = used;
        i = e;
    }
    list.line_last = int(last_used) + 1;
    return i;
}

// `#+BEGIN_x` .. `#+END_x` at line `i`; returns the line after the block.
size_t parse_begin_block(Ctx& c, size_t i, size_t to, const std::string& name, const std::string& args, std::vector<Block>& out) {
    size_t end = i + 1;
    while (end < to) {
        std::string_view t = trim(c.lines[end]);
        if (starts_ci(t, "#+end_") && lower(trim(t.substr(6))) == name) break;
        end++;
    }
    const bool closed = end < to;
    const int first = int(i) + 1, last = closed ? int(end) + 1 : int(to);
    const size_t cfrom = i + 1, cto = end;  // content lines
    const size_t strip = indent_of(c.lines[i]);
    auto raw_text = [&] {
        std::string text;
        for (size_t k = cfrom; k < cto; k++) {
            std::string_view l = c.lines[k];
            l.remove_prefix(std::min(strip, indent_of(l)));
            if (k > cfrom) text += '\n';
            text += l;
        }
        return text;
    };
    if (name == "comment" || name == "export" || name == "notes") {
        // hidden (notes: speaker notes, spec 26; slides mode shows them outside the slide)
    } else if (name == "src" || name == "example") {
        Block b;
        b.kind = K::Code;
        if (name == "src") {
            std::string_view a = args;
            b.lang = std::string(a.substr(0, a.find_first_of(" \t")));
        }
        b.text = raw_text();
        b.line_first = first, b.line_last = last;
        out.push_back(std::move(b));
    } else if (name == "quote" || name == "note" || name == "tip" || name == "important" || name == "warning" || name == "caution") {
        Block b;
        b.kind = K::Quote;
        if (name != "quote") b.callout = name, b.title_line = first;
        b.children = parse_blocks(c, cfrom, cto);
        b.line_first = first, b.line_last = last;
        out.push_back(std::move(b));
    } else if (name == "verse") {
        Spans spans;
        for (size_t k = cfrom; k < cto; k++) {
            if (blank_line(c.lines[k])) continue;
            if (!spans.empty()) spans.push_back(Span{"\n"});
            Spans l = inline_spans(trim(c.lines[k]), c);
            for (Span& s : l) spans.push_back(std::move(s));
        }
        if (!spans.empty()) out.push_back(make_paragraph(std::move(spans), first, last));
    } else {  // CENTER and unknown names: the content as normal blocks
        for (Block& b : parse_blocks(c, cfrom, cto)) out.push_back(std::move(b));
    }
    return closed ? end + 1 : to;
}

std::vector<Block> parse_blocks(Ctx& c, size_t from, size_t to) {
    std::vector<Block> out;
    bool plan_ok = false;  // right after a headline: planning lines and drawers are hidden
    size_t i = from;
    while (i < to) {
        const std::string& line = c.lines[i];
        const std::string_view tl = trim(line);
        if (tl.empty()) {
            i++;
            continue;
        }
        const bool was_plan_ok = plan_ok;
        plan_ok = false;
        std::string name, args;
        if (begin_line(line, name, args)) {
            i = parse_begin_block(c, i, to, name, args, out);
            continue;
        }
        if (starts_ci(tl, "#+title:")) {
            Block h;
            h.kind = K::Heading;
            h.level = 1;
            h.spans = inline_spans(trim(tl.substr(8)), c);
            h.line_first = h.line_last = int(i) + 1;
            out.push_back(std::move(h));
            i++;
            continue;
        }
        if (tl.substr(0, 2) == "#+" || is_comment(line)) {  // other keywords, comments, stray #+END_x
            i++;
            continue;
        }
        if (int lvl = headline_level(line)) {
            std::string_view rest = trim(std::string_view(line).substr(size_t(lvl)));
            // Trailing tags `:a:b:` after whitespace.
            if (rest.size() > 2 && rest.back() == ':') {
                const size_t sp = rest.find_last_of(" \t");
                if (sp != std::string_view::npos) {
                    std::string_view tags = rest.substr(sp + 1);
                    bool ok = tags.size() > 2 && tags.front() == ':';
                    for (char ch : tags)
                        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '@' && ch != '#' && ch != '%' && ch != ':') ok = false;
                    if (ok) rest = trim(rest.substr(0, sp));
                }
            }
            Block h;
            h.kind = K::Heading;
            h.level = std::min(lvl, 6);
            for (const char* kw : {"TODO", "DONE", "NEXT", "WAIT", "CANCELLED"}) {
                const std::string_view k = kw;
                if (rest.substr(0, k.size()) == k && (rest.size() == k.size() || rest[k.size()] == ' ')) {
                    Span b;
                    b.text = std::string(k);
                    b.bold = true;
                    h.spans.push_back(std::move(b));
                    rest = rest.substr(k.size());
                    if (!rest.empty()) h.spans.push_back(Span{" "});
                    rest = trim(rest);
                    break;
                }
            }
            for (Span& s : inline_spans(rest, c)) h.spans.push_back(std::move(s));
            h.line_first = h.line_last = int(i) + 1;
            out.push_back(std::move(h));
            plan_ok = true;
            i++;
            continue;
        }
        if (was_plan_ok && planning_line(tl)) {
            plan_ok = true;
            i++;
            continue;
        }
        if (drawer_start(tl)) {  // :NAME: .. :END:
            size_t e = i + 1;
            while (e < to && lower(trim(c.lines[e])) != ":end:") e++;
            if (e < to) {
                plan_ok = was_plan_ok;
                i = e + 1;
                continue;
            }
        }
        std::string label;
        size_t text_at = 0;
        if (footnote_def(line, label, text_at)) {  // definitions are collected up front; no block of their own
            i++;
            while (i < to && !blank_line(c.lines[i]) && !headline_level(c.lines[i]) && !footnote_def(c.lines[i], label, text_at)) i++;
            continue;
        }
        if (is_fixed_width(line)) {
            Block b;
            b.kind = K::Code;
            b.line_first = int(i) + 1;
            while (i < to && is_fixed_width(c.lines[i])) {
                std::string_view l = c.lines[i];
                l = l.substr(indent_of(l));
                if (!b.text.empty() || b.line_last) b.text += '\n';
                b.text += l.size() > 2 ? l.substr(2) : std::string_view();
                b.line_last = int(i) + 1;
                i++;
            }
            out.push_back(std::move(b));
            continue;
        }
        if (is_rule(tl)) {
            Block r;
            r.kind = K::Rule;
            r.line_first = r.line_last = int(i) + 1;
            out.push_back(std::move(r));
            i++;
            continue;
        }
        if (is_table_line(tl)) {
            size_t e = i;
            while (e < to && is_table_line(c.lines[e])) e++;
            out.push_back(make_table(c, i, e));
            i = e;
            continue;
        }
        if (math_start(tl)) {
            const bool env = starts_ci(tl, "\\begin{");
            const bool bracket = tl.substr(0, 2) == "\\[";
            size_t e = i;
            std::string tex;
            bool closed = false;
            if (env) {
                const size_t close = tl.find('}');
                const std::string envname = close == std::string_view::npos ? "" : std::string(tl.substr(7, close - 7));
                const std::string endtag = "\\end{" + envname + "}";
                for (; e < to; e++) {
                    if (e > i) tex += '\n';
                    tex += trim(c.lines[e]);
                    if (trim(c.lines[e]).find(endtag) != std::string_view::npos) {
                        closed = true;
                        break;
                    }
                }
            } else {
                const std::string endtag = bracket ? "\\]" : "$$";
                std::string all;
                for (; e < to; e++) {
                    all += (e > i ? "\n" : "");
                    all += trim(c.lines[e]);
                    if (all.find(endtag, 2) != std::string::npos) {
                        closed = true;
                        break;
                    }
                }
                if (closed) tex = std::string(trim(std::string_view(all).substr(2, all.find(endtag, 2) - 2)));
            }
            if (closed) {
                Block m;
                m.kind = K::Math;
                m.text = tex;
                m.line_first = int(i) + 1;
                m.line_last = int(e) + 1;
                out.push_back(std::move(m));
                i = e + 1;
                continue;
            }
            // not closed: falls through to a paragraph
        }
        Bullet bl;
        if (parse_bullet(line, bl)) {
            Block list;
            i = parse_list(c, i, to, list);
            out.push_back(std::move(list));
            continue;
        }
        // Paragraph.
        Spans spans;
        const size_t pfirst = i;
        add_line(spans, line, true, c);
        i++;
        while (i < to && !blank_line(c.lines[i]) && !starts_element(c.lines[i])) {
            add_line(spans, c.lines[i], false, c);
            i++;
        }
        // A trailing hard break at the end of the paragraph is dropped.
        if (!spans.empty() && spans.back().text == "\n" && !spans.back().math) spans.pop_back();
        out.push_back(make_paragraph(std::move(spans), int(pfirst) + 1, int(i)));
    }
    return out;
}

}  // namespace

bool is_org_path(std::string_view path) {
    const size_t dot = path.rfind('.');
    return dot != std::string_view::npos && lower(path.substr(dot)) == ".org";
}

Document parse_format(std::string_view src, std::string_view format) {
    return format == "org" ? parse_org(src) : parse(src);
}

Document parse_org(std::string_view src) {
    Ctx c;
    for (size_t pos = 0; pos <= src.size();) {
        size_t e = src.find('\n', pos);
        if (e == std::string_view::npos) e = src.size();
        std::string l(src.substr(pos, e - pos));
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (pos == 0 && l.compare(0, 3, "\xEF\xBB\xBF") == 0) l.erase(0, 3);  // BOM
        c.lines.push_back(std::move(l));
        pos = e + 1;
    }
    if (!c.lines.empty() && c.lines.back().empty()) c.lines.pop_back();  // text ending in "\n"
    // Footnote definitions first, so references know which labels exist.
    for (size_t i = 0; i < c.lines.size(); i++) {
        std::string label;
        size_t at = 0;
        if (!footnote_def(c.lines[i], label, at)) continue;
        std::string text(trim(std::string_view(c.lines[i]).substr(at)));
        for (size_t j = i + 1; j < c.lines.size() && !blank_line(c.lines[j]) && !headline_level(c.lines[j]); j++) {
            std::string l2;
            size_t at2;
            if (footnote_def(c.lines[j], l2, at2)) break;
            text += " " + std::string(trim(c.lines[j]));
        }
        c.fn_defs.emplace(label, text);
    }
    Document doc = parse_blocks(c, 0, c.lines.size());
    lift_display_blocks(doc);

    if (!c.fn_order.empty()) {  // the footnote section, like the Markdown parser's: a rule and one paragraph each
        const int tail_line = doc.empty() ? 1 : doc.back().line_last;
        Block rule;
        rule.kind = K::Rule;
        rule.line_first = rule.line_last = tail_line;
        doc.push_back(std::move(rule));
        for (size_t i = 0; i < c.fn_order.size(); i++) {  // (the text may reference more footnotes: the loop grows)
            Span num;
            num.text = std::to_string(i + 1) + ". ";
            Block para;
            para.kind = K::Paragraph;
            para.footnote = int(i + 1);
            para.line_first = para.line_last = tail_line;
            para.spans.push_back(std::move(num));
            for (Span& s : inline_spans(c.fn_defs[c.fn_order[i]], c)) para.spans.push_back(std::move(s));
            Span back;
            back.text = " \xE2\x86\xA9";  // " ↩"
            back.link = "#fnref-" + std::to_string(i + 1);
            para.spans.push_back(std::move(back));
            doc.push_back(std::move(para));
        }
    }
    return doc;
}

}  // namespace md
