#include "slides.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>

namespace slides {

std::vector<Range> split(const md::Document& doc) {
    using K = md::Block::Kind;
    std::vector<Range> out;
    const size_t n = doc.size();
    if (n == 0) return out;
    const bool has_rule = std::any_of(doc.begin(), doc.end(), [](const md::Block& b) { return b.kind == K::Rule; });
    if (has_rule) {
        size_t start = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i < n && doc[i].kind != K::Rule) continue;
            if (i > start) out.push_back({start, i - 1});
            start = i + 1;
        }
        return out;
    }
    int level = 7;
    for (const md::Block& b : doc)
        if (b.kind == K::Heading) level = std::min(level, b.level);
    size_t start = 0;
    for (size_t i = 1; i < n; i++) {
        if (doc[i].kind != K::Heading || doc[i].level != level) continue;
        out.push_back({start, i - 1});
        start = i;
    }
    out.push_back({start, n - 1});
    return out;
}

std::vector<Range> split_with_ends(const md::Document& doc, const std::vector<int>& ends) {
    if (ends.empty()) return split(doc);
    std::vector<Range> out;
    size_t start = 0;
    for (const int end : ends) {
        size_t cut = doc.size();
        for (size_t i = 0; i < doc.size() && cut == doc.size(); i++)
            if (doc[i].line_first > end) cut = i;
        if (cut > start) out.push_back({start, cut - 1}), start = cut;
    }
    if (doc.size() > start) out.push_back({start, doc.size() - 1});
    return out;
}

size_t slide_of_block(const std::vector<Range>& slides, size_t block) {
    size_t best = 0;
    for (size_t i = 0; i < slides.size(); i++)
        if (slides[i].first <= block) best = i;
    return best;
}

std::pair<int, int> slide_lines(const std::vector<Range>& slides, const md::Document& doc, size_t n, int line_count) {
    const int first = n < slides.size() && slides[n].first < doc.size() ? doc[slides[n].first].line_first : 1;
    int last = line_count;
    if (n + 1 < slides.size() && slides[n + 1].first < doc.size()) last = doc[slides[n + 1].first].line_first - 1;
    return {first, std::max(first, last)};
}

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool starts_with_nocase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++)
        if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
    return true;
}

// Calls `fn(line, body)` for every directive comment of `src`: a Markdown line that is just
// `<!-- body -->` (indented by at most 3 spaces) or an Org comment line `# body` (not indented),
// outside fenced blocks (Markdown ``` / ~~~, Org #+begin_ ... #+end_). `body` is trimmed.
void comment_lines(std::string_view src, bool org, const std::function<void(int, std::string_view)>& fn) {
    int n = 0;
    char fence_ch = 0;      // Markdown: the open fence's char (0 = none) and length
    size_t fence_len = 0;
    bool in_block = false;  // Org: inside #+begin_ ... #+end_
    bool prev_blank = true;  // Markdown: the line before was blank (or there was none)
    while (!src.empty()) {
        const size_t nl = src.find('\n');
        const std::string_view raw = src.substr(0, nl);
        src = nl == std::string_view::npos ? std::string_view() : src.substr(nl + 1);
        n++;
        const size_t indent = std::min(raw.find_first_not_of(' '), raw.size());
        const std::string_view line = trim(raw);
        const bool blank_before = prev_blank;
        prev_blank = line.empty();
        if (org) {
            if (starts_with_nocase(line, "#+begin_")) in_block = true;
            else if (starts_with_nocase(line, "#+end_")) in_block = false;
            else if (!in_block && indent == 0 && line.size() > 2 && line[0] == '#' && line[1] == ' ')
                fn(n, trim(line.substr(1)));
            continue;
        }
        if (indent <= 3 && !line.empty() && (line[0] == '`' || line[0] == '~')) {
            const char ch = line[0];
            const size_t run = std::min(line.find_first_not_of(ch), line.size());
            if (run >= 3) {
                if (!fence_ch) fence_ch = ch, fence_len = run;
                else if (ch == fence_ch && run >= fence_len && run == line.size()) fence_ch = 0;
                continue;
            }
        }
        const bool next_blank = trim(src.substr(0, src.find('\n'))).empty();
        if (!fence_ch && indent <= 3 && line.size() >= 7 && line.substr(0, 4) == "<!--" && line.substr(line.size() - 3) == "-->")
            fn(n, trim(line.substr(4, line.size() - 7)));
        else if (!fence_ch && indent <= 3 && line == ". . ." && blank_before && next_blank)
            fn(n, "pause");  // pandoc's pause: a paragraph of its own
    }
}

// "12" -> 12; anything but digits (or too many of them) -> -1.
int parse_uint(std::string_view s) {
    if (s.empty() || s.size() > 6) return -1;
    int v = 0;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') return -1;
        v = v * 10 + (ch - '0');
    }
    return v;
}

// `body` after `key` and a colon (spaces around it allowed), or nullopt.
std::optional<std::string_view> after_key(std::string_view body, std::string_view key) {
    if (body.substr(0, key.size()) != key) return std::nullopt;
    body = trim(body.substr(key.size()));
    if (body.empty() || body[0] != ':') return std::nullopt;
    return trim(body.substr(1));
}

}  // namespace

std::vector<int> pause_lines(std::string_view src, bool org) {
    std::vector<int> out;
    comment_lines(src, org, [&](int line, std::string_view body) {
        if (body == "pause") out.push_back(line);
    });
    return out;
}

std::vector<Directive> layout_lines(std::string_view src, bool org) {
    using K = Directive::Kind;
    std::vector<Directive> out;
    comment_lines(src, org, [&](int line, std::string_view body) {
        if (body == "reset_layout") {
            out.push_back({line, K::Reset, {}, 0});
        } else if (auto list = after_key(body, "column_layout")) {
            // `[a, b, ...]`: 2 to 4 positive integers.
            if (list->size() < 2 || list->front() != '[' || list->back() != ']') return;
            std::string_view rest = list->substr(1, list->size() - 2);
            std::vector<int> weights;
            while (true) {
                const size_t comma = rest.find(',');
                const int w = parse_uint(trim(rest.substr(0, comma)));
                if (w <= 0) return;
                weights.push_back(w);
                if (comma == std::string_view::npos) break;
                rest = rest.substr(comma + 1);
            }
            if (weights.size() >= 2 && weights.size() <= 4) out.push_back({line, K::Layout, std::move(weights), 0});
        } else if (auto n = after_key(body, "column")) {
            if (const int c = parse_uint(*n); c >= 0) out.push_back({line, K::Column, {}, c});
        }
    });
    return out;
}

std::vector<Placement> placements(const std::vector<Range>& slides, const md::Document& doc, const std::vector<Directive>& directives) {
    using K = Directive::Kind;
    // Each directive applies from the next top-level block on (doc.size(): none follows). A rule
    // between slides is in no slide: a directive just above it goes to the next slide.
    auto in_slide = [&](size_t i) { return std::any_of(slides.begin(), slides.end(), [&](const Range& r) { return r.first <= i && i <= r.last; }); };
    std::vector<size_t> target;
    for (const Directive& d : directives) {
        size_t next = doc.size();
        for (size_t i = 0; i < doc.size() && next == doc.size(); i++)
            if (doc[i].line_first > d.line && in_slide(i)) next = i;
        target.push_back(next);
    }
    std::vector<Placement> out(doc.size());
    std::vector<int> weights;  // the open layout's (empty: none)
    int layout = -1, row = -1, column = 0, layouts = 0;
    bool any = false;
    for (size_t i = 0; i < doc.size(); i++) {
        // A slide's first block: the previous slide's layout ends there.
        if (std::any_of(slides.begin(), slides.end(), [&](const Range& r) { return r.first == i; })) weights.clear(), row = -1;
        for (size_t k = 0; k < directives.size(); k++) {
            if (target[k] != i) continue;
            const Directive& d = directives[k];
            if (d.kind == K::Layout) {
                weights = d.weights, layout = layouts++, row = -1;  // full width until a `column:`
            } else if (d.kind == K::Reset) {
                weights.clear(), row = -1;
            } else if (d.column < int(weights.size())) {
                row = layout, column = d.column;
            }
        }
        if (row >= 0) out[i] = {row, column, weights}, any = true;
    }
    if (!any) out.clear();
    return out;
}

std::vector<int> end_slide_lines(std::string_view src, bool org) {
    std::vector<int> out;
    comment_lines(src, org, [&](int line, std::string_view body) {
        if (body == "end_slide") out.push_back(line);
    });
    return out;
}

std::vector<std::pair<int, bool>> incremental_lines(std::string_view src, bool org) {
    std::vector<std::pair<int, bool>> out;
    comment_lines(src, org, [&](int line, std::string_view body) {
        if (auto v = after_key(body, "incremental_lists")) {
            if (*v == "true") out.push_back({line, true});
            else if (*v == "false") out.push_back({line, false});
        }
    });
    return out;
}

std::vector<std::vector<Step>> steps(const std::vector<Range>& slides, const md::Document& doc, const std::vector<int>& pauses,
                                     const std::vector<std::pair<int, bool>>& incremental) {
    std::vector<std::vector<Step>> out(slides.size());
    for (int p : pauses) {
        size_t next = doc.size();  // the step starts at the next top-level block
        for (size_t i = 0; i < doc.size() && next == doc.size(); i++)
            if (doc[i].line_first > p) next = i;
        if (next == doc.size()) continue;
        for (size_t s = 0; s < slides.size(); s++) {
            if (slides[s].first < next && next <= slides[s].last) {
                const Step step{next, 0};
                if (std::find(out[s].begin(), out[s].end(), step) == out[s].end()) out[s].push_back(step);
                break;
            }
        }
    }
    // Like column directives: a switch applies from the next block of a slide below it.
    auto in_slide = [&](size_t i) { return std::any_of(slides.begin(), slides.end(), [&](const Range& r) { return r.first <= i && i <= r.last; }); };
    std::vector<std::pair<size_t, bool>> switches;  // (block, on)
    for (const auto& [line, on] : incremental) {
        for (size_t i = 0; i < doc.size(); i++) {
            if (doc[i].line_first > line && in_slide(i)) {
                switches.push_back({i, on});
                break;
            }
        }
    }
    for (size_t s = 0; s < slides.size(); s++) {
        bool on = false;  // each slide starts with them off
        for (size_t i = slides[s].first; i <= slides[s].last; i++) {
            for (const auto& [b, v] : switches)
                if (b == i) on = v;
            if (on && doc[i].kind == md::Block::Kind::List)
                for (size_t k = 1; k < doc[i].items.size(); k++) out[s].push_back({i, k});
        }
    }
    for (auto& v : out) std::sort(v.begin(), v.end());
    return out;
}

size_t step_of_block(const std::vector<Step>& steps, size_t block) {
    return size_t(std::count_if(steps.begin(), steps.end(), [&](const Step& st) { return st.first <= block; }));
}

std::vector<TextDirective> text_lines(std::string_view src, bool org) {
    using K = TextDirective::Kind;
    std::vector<TextDirective> out;
    comment_lines(src, org, [&](int line, std::string_view body) {
        if (body == "newline" || body == "new_line") {
            out.push_back({line, K::NewLines, 1});
        } else if (auto v = after_key(body, "new_lines") ? after_key(body, "new_lines") : after_key(body, "newlines")) {
            if (const int n = parse_uint(*v); n >= 1 && n <= 50) out.push_back({line, K::NewLines, n});
        } else if (auto a = after_key(body, "alignment")) {
            if (*a == "left") out.push_back({line, K::Align, 0, TextAlign::Left});
            else if (*a == "center") out.push_back({line, K::Align, 0, TextAlign::Center});
            else if (*a == "right") out.push_back({line, K::Align, 0, TextAlign::Right});
        } else if (auto f = after_key(body, "font_size")) {
            if (const int n = parse_uint(*f); n >= 1 && n <= 7) out.push_back({line, K::FontSize, n});
        }
    });
    return out;
}

std::vector<TextStyle> text_styles(const std::vector<Range>& slides, const md::Document& doc, const std::vector<TextDirective>& directives) {
    using K = TextDirective::Kind;
    auto in_slide = [&](size_t i) { return std::any_of(slides.begin(), slides.end(), [&](const Range& r) { return r.first <= i && i <= r.last; }); };
    // Each directive's block: the next block of a slide below it (doc.size(): none).
    std::vector<size_t> target;
    for (const TextDirective& d : directives) {
        size_t next = doc.size();
        for (size_t i = 0; i < doc.size() && next == doc.size(); i++)
            if (doc[i].line_first > d.line && in_slide(i)) next = i;
        target.push_back(next);
    }
    std::vector<TextStyle> out(doc.size());
    for (const Range& r : slides) {
        TextStyle now;
        for (size_t i = r.first; i <= r.last && i < doc.size(); i++) {
            now.lines_before = 0;
            for (size_t k = 0; k < directives.size(); k++) {
                if (target[k] != i) continue;
                const TextDirective& d = directives[k];
                if (d.kind == K::NewLines) now.lines_before += d.n;
                else if (d.kind == K::Align) now.align = d.align;
                else now.font_size = d.n;
            }
            out[i] = now;
        }
    }
    return out;
}

std::vector<std::pair<int, std::string>> note_lines(std::string_view src, bool org) {
    std::vector<std::string_view> lines;
    while (!src.empty()) {
        const size_t nl = src.find('\n');
        lines.push_back(src.substr(0, nl));
        src = nl == std::string_view::npos ? std::string_view() : src.substr(nl + 1);
    }
    // The trimmed text lines from `from` up to the first line where `is_end` holds, joined; and that line.
    auto body = [&](size_t from, auto is_end) {
        size_t end = from;
        while (end < lines.size() && !is_end(trim(lines[end]))) end++;
        std::string text;
        for (size_t k = from; k < end; k++) {
            if (k > from) text += '\n';
            text += trim(lines[k]);
        }
        // (blank lines at either end dropped)
        const size_t lead = text.find_first_not_of('\n');
        if (lead == std::string::npos) return std::make_pair(std::string(), end);
        return std::make_pair(text.substr(lead, text.find_last_not_of('\n') + 1 - lead), end);
    };
    auto lower = [](std::string_view s) {
        std::string out(s);
        for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    std::vector<std::pair<int, std::string>> out;
    char fence_ch = 0;
    size_t fence_len = 0;
    bool in_block = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string_view raw = lines[i];
        const size_t indent = std::min(raw.find_first_not_of(' '), raw.size());
        const std::string_view line = trim(raw);
        if (org) {
            const std::string l = lower(line);
            if (!in_block && indent == 0 && l == "#+begin_notes") {
                auto [text, end] = body(i + 1, [&](std::string_view t) { return lower(t) == "#+end_notes"; });
                out.push_back({int(i + 1), text});
                i = end;
            } else if (l.rfind("#+begin_", 0) == 0) {
                in_block = true;
            } else if (l.rfind("#+end_", 0) == 0) {
                in_block = false;
            }
            continue;
        }
        if (indent <= 3 && !line.empty() && (line[0] == '`' || line[0] == '~')) {
            const char ch = line[0];
            const size_t run = std::min(line.find_first_not_of(ch), line.size());
            if (run >= 3) {
                if (!fence_ch) fence_ch = ch, fence_len = run;
                else if (ch == fence_ch && run >= fence_len && run == line.size()) fence_ch = 0;
                continue;
            }
        }
        if (fence_ch || indent > 3 || line.substr(0, 4) != "<!--") continue;
        const auto rest = after_key(trim(line.substr(4)), "speaker_note");
        if (!rest) continue;
        if (rest->size() >= 3 && rest->substr(rest->size() - 3) == "-->") {
            out.push_back({int(i + 1), std::string(trim(rest->substr(0, rest->size() - 3)))});
        } else if (*rest == "|") {
            auto [text, end] = body(i + 1, [](std::string_view t) { return t == "-->"; });
            out.push_back({int(i + 1), text});
            i = end;
        }
    }
    return out;
}

std::vector<std::string> slide_notes(const std::vector<Range>& slides, const md::Document& doc, const std::vector<std::pair<int, std::string>>& notes) {
    std::vector<std::string> out(slides.size());
    for (const auto& [line, text] : notes) {
        if (text.empty() || slides.empty()) continue;
        size_t block = 0;  // the block at or above the note
        for (size_t i = 0; i < doc.size(); i++)
            if (doc[i].line_first <= line) block = i;
        // Its slide; a rule between slides (in none): the next one.
        size_t s_i = slides.size() - 1;
        for (size_t k = slides.size(); k-- > 0;)
            if (slides[k].last >= block) s_i = k;
        std::string& s = out[s_i];
        if (!s.empty()) s += "\n\n";
        s += text;
    }
    return out;
}

float zoom(float w, float h_win, float h) {
    const float by_width = std::min(w / 760.0f, 3.0f);  // 700-unit column + margins; at most 3x
    return std::max(std::min(by_width, h_win / (h + 80.0f)), 0.6f * by_width);
}

std::optional<Background> background_of(std::string_view alt, std::string_view src) {
    std::vector<std::string_view> words;
    for (size_t i = 0; i < alt.size();) {
        while (i < alt.size() && std::isspace(static_cast<unsigned char>(alt[i]))) i++;
        size_t j = i;
        while (j < alt.size() && !std::isspace(static_cast<unsigned char>(alt[j]))) j++;
        if (j > i) words.push_back(alt.substr(i, j - i));
        i = j;
    }
    if (words.empty() || words[0] != "bg") return std::nullopt;
    Background bg{std::string(src)};
    for (size_t k = 1; k < words.size(); k++) {
        if (auto f = imagefilter::parse_filter(words[k])) {
            bg.filters.push_back(*f);
            continue;
        }
        std::string_view side = words[k], size;
        const bool sized = side.find(':') != std::string_view::npos;
        if (sized) size = side.substr(side.find(':') + 1), side = side.substr(0, side.find(':'));
        float frac = 0.5f;
        if (sized) {
            if (size.empty() || size.back() != '%') return std::nullopt;
            size.remove_suffix(1);
            while (!size.empty() && size.front() == ' ') size.remove_prefix(1);
            while (!size.empty() && size.back() == ' ') size.remove_suffix(1);
            char* end = nullptr;
            const std::string num(size);
            const float v = std::strtof(num.c_str(), &end);
            if (num.empty() || end != num.c_str() + num.size()) return std::nullopt;
            frac = std::clamp(v / 100.0f, 0.1f, 0.9f);
        }
        if (side == "left") bg.side = BgSide::Left, bg.frac = frac;
        else if (side == "right") bg.side = BgSide::Right, bg.frac = frac;
        else if ((side == "contain" || side == "fit") && !sized) bg.contain = true;
        else if (side == "cover" && !sized) bg.contain = false;
        else if (side == "vertical" && !sized) bg.vertical = true;
        else return std::nullopt;
    }
    return bg;
}

std::vector<Background> block_backgrounds(const md::Block& b) {
    if (b.kind != md::Block::Kind::Images) return {};
    std::vector<Background> out;
    for (const md::ImageRef& im : b.images)
        if (auto bg = background_of(im.alt, im.src)) out.push_back(*bg);
    if (out.size() != b.images.size()) out.clear();
    return out;
}

std::vector<std::vector<Background>> slide_backgrounds(const std::vector<Range>& slides, const md::Document& doc) {
    std::vector<std::vector<Background>> out;
    for (const Range& r : slides) {
        std::vector<Background> found;
        for (size_t i = r.first; i <= r.last && i < doc.size(); i++)
            for (Background& bg : block_backgrounds(doc[i])) found.push_back(std::move(bg));
        out.push_back(std::move(found));
    }
    return out;
}

const Background* backgrounds_strip(const std::vector<Background>& bgs) {
    for (const Background& bg : bgs)
        if (bg.side != BgSide::Full) return &bg;
    return nullptr;
}

std::vector<std::array<float, 4>> background_cells(const std::vector<Background>& bgs, std::array<float, 4> area) {
    const float n = float(std::max<size_t>(bgs.size(), 1));
    const auto [x, y, w, h] = area;
    const bool vertical = std::any_of(bgs.begin(), bgs.end(), [](const Background& b) { return b.vertical; });
    std::vector<std::array<float, 4>> out;
    for (size_t k = 0; k < bgs.size(); k++) {
        const float i = float(k);
        out.push_back(vertical ? std::array<float, 4>{x, y + h * i / n, w, h / n} : std::array<float, 4>{x + w * i / n, y, w / n, h});
    }
    return out;
}

std::optional<BgSplit> background_split(const Background& bg) {
    if (bg.side == BgSide::Full) return std::nullopt;
    const int p = int(std::lround(bg.frac * 100.0f));
    BgSplit out;
    out.weights = bg.side == BgSide::Left ? std::vector<int>{p, 100 - p} : std::vector<int>{100 - p, p};
    out.text_column = bg.side == BgSide::Left ? 1 : 0;
    // Column 1 starts after column 0 and the 32-unit gap (render.cpp's column math, at 700 units).
    const float room = 700.0f - 32.0f;
    out.edge = room * float(out.weights[0]) / 100.0f + 32.0f / 2.0f;
    return out;
}

void place_backgrounds(std::vector<Placement>& places, const std::vector<Range>& slides,
                       const std::vector<std::vector<Background>>& bgs, size_t blocks) {
    for (size_t n = 0; n < slides.size() && n < bgs.size(); n++) {
        const Background* strip = backgrounds_strip(bgs[n]);
        const auto split = strip ? background_split(*strip) : std::nullopt;
        if (!split) continue;
        if (places.empty()) places.assign(blocks, Placement{});
        for (size_t i = slides[n].first; i <= slides[n].last && i < places.size(); i++)
            if (places[i].row < 0) places[i] = Placement{1000000 + int(n), split->text_column, split->weights};  // (ids far above the slides' own)
    }
}

Grid overview_grid(size_t n, float w, float h, float aspect, float gap, float label) {
    n = std::max<size_t>(n, 1);
    aspect = std::max(aspect, 0.01f);
    Grid best;
    for (size_t cols = 1; cols <= n; cols++) {
        const size_t rows = (n + cols - 1) / cols;
        const float by_width = (w - gap * float(cols + 1)) / float(cols);
        const float by_height = ((h - gap * float(rows + 1)) / float(rows) - label) * aspect;
        const float cell_w = std::min(by_width, by_height);
        if (cell_w > best.cell_w) best = {cols, cell_w, cell_w / aspect};
    }
    return best;
}

Grid overview_grid_min(size_t n, float w, float h, float aspect, float gap, float label, float min_w) {
    const Grid fit = overview_grid(n, w, h, aspect, gap, label);
    if (fit.cell_w >= min_w) return fit;
    const size_t cols = std::clamp<size_t>(size_t(std::max(std::floor((w - gap) / (min_w + gap)), 0.0f)), 1, std::max<size_t>(n, 1));
    const float cell_w = std::max((w - gap * float(cols + 1)) / float(cols), 1.0f);
    return {cols, cell_w, cell_w / std::max(aspect, 0.01f)};
}

float overview_height(size_t n, size_t cols, float cell_h, float gap, float label) {
    const size_t rows = (std::max<size_t>(n, 1) + std::max<size_t>(cols, 1) - 1) / std::max<size_t>(cols, 1);
    return float(rows) * (cell_h + label) + float(rows + 1) * gap;
}

float overview_follow(float scroll, float top, float bottom, float view_h, float gap) {
    if (top - gap < scroll) return top - gap;
    if (bottom + gap > scroll + view_h) return bottom + gap - view_h;
    return scroll;
}

size_t overview_move(size_t at, int dx, int dy, size_t cols, size_t n) {
    const long long last = (long long)std::max<size_t>(n, 1) - 1;
    const long long to = (long long)at + dx + (long long)dy * (long long)std::max<size_t>(cols, 1);
    if (dy != 0 && (to < 0 || to > last)) return at;  // no row there
    return size_t(std::clamp(to, 0LL, last));
}

PresenterLayout presenter_layout(float w, float h, float aspect) {
    const float pad = kPresenterPad, bar = kPresenterBar, label = kPresenterLabel;
    aspect = std::max(aspect, 0.1f);
    const float split = std::round(w * 0.62f);
    PresenterLayout l;
    l.bar = bar;
    // The current slide: as big as fits on the left.
    const float room_w = std::max(split - 1.5f * pad, 1.0f), room_h = std::max(h - bar - label - pad, 1.0f);
    const float now_w = std::min(room_w, room_h * aspect);
    l.now[0] = pad, l.now[1] = bar + label, l.now[2] = now_w, l.now[3] = now_w / aspect;
    // The next one: the right column's width, at most 40% of the height.
    const float x = split + pad / 2;
    const float col_w = std::max(w - x - pad, 1.0f);
    const float next_w = std::min(col_w, std::max((h - bar) * 0.4f - label, 1.0f) * aspect);
    l.next[0] = x, l.next[1] = bar + label, l.next[2] = next_w, l.next[3] = next_w / aspect;
    // The notes: the rest of the right column.
    const float top = l.next[1] + l.next[3] + pad + label;
    l.notes[0] = x, l.notes[1] = top, l.notes[2] = col_w, l.notes[3] = std::max(h - pad - top, 0.0f);
    return l;
}

}  // namespace slides
