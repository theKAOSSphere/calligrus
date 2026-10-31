// Syntax highlighting with the tree-sitter C library.
//
// This is a line-by-line port of the Rust port (read its header
// comment for the algorithm, steps 1-5). The grammars, queries, theme.txt and
// languages.txt are the same files (the Rust port, embedded at build
// time), and calligrus-highlight-test checks the result against the same golden
// files as the Rust tests. If you change the algorithm here, change it there.
//
// One thing the Rust tree-sitter crate does that the C library doesn't: the
// text predicates (#eq? #match? #any-of? and variants) in queries. They are
// implemented below (`Predicate`, `satisfies`) exactly like the crate does.
#include <cstdint>
#include "highlight.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <thread>
#include <tuple>

#include "highlight_embedded.h"
#include "syntax_entries.h"

namespace {

using Clock = std::chrono::steady_clock;

// Parsing + querying a block must finish within this time, else: plain.
constexpr auto kTimeBudget = std::chrono::milliseconds(500);
// Marks a byte that no capture claimed.
constexpr size_t kNoStyle = SIZE_MAX;

// --- small text helpers (same rules as Rust's lines() / trim()) ----------------

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split_words(std::string_view s) {
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) i++;
        size_t start = i;
        while (i < s.size() && !is_space(s[i])) i++;
        if (i > start) words.push_back(s.substr(start, i - start));
    }
    return words;
}

// The non-blank, non-`#` lines of an embedded text file, trimmed.
std::vector<std::string_view> content_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        line = trim(line);
        if (!line.empty() && line[0] != '#') lines.push_back(line);
    }
    return lines;
}

// --- syntax entries ----------------------------------------------------------------

// theme.txt's entry names, parsed once (see syntax_entries.h). The colours
// come from the active theme at draw time, not from here.
const std::vector<SyntaxEntry>& entries() {
    static const std::vector<SyntaxEntry> list = parse_syntax_entries({kEmbeddedThemeTxt, kEmbeddedThemeTxtLen});
    return list;
}

// --- languages -----------------------------------------------------------------

// Which embedded grammar a fence info string means: its first word, up to a
// `,`, ASCII-lowercased, looked up among the names and aliases of languages.txt.
std::optional<size_t> find_language(std::string_view lang) {
    auto words = split_words(lang);
    std::string word(words.empty() ? std::string_view() : words[0].substr(0, words[0].find(',')));
    for (char& c : word)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (word.empty()) return std::nullopt;

    static const std::vector<std::pair<std::string, size_t>> aliases = [] {
        std::vector<std::pair<std::string, size_t>> list;
        for (std::string_view line : content_lines({kEmbeddedLanguagesTxt, kEmbeddedLanguagesTxtLen})) {
            auto w = split_words(line);
            for (size_t g = 0; g < kEmbeddedGrammarCount; g++) {
                if (w[0] != kEmbeddedGrammars[g].name) continue;
                for (std::string_view alias : w) list.emplace_back(std::string(alias), g);
            }
        }
        return list;
    }();
    for (const auto& [alias, index] : aliases)
        if (alias == word) return index;
    return std::nullopt;
}

// --- query predicates (what the Rust crate does in satisfies_text_predicates) ---

struct Predicate {
    enum Kind { EqCapture, EqString, MatchString, AnyString } kind;
    uint32_t capture = 0, capture2 = 0;  // capture ids
    std::string text;                     // EqString
    std::shared_ptr<std::regex> regex;    // MatchString
    std::vector<std::string> values;      // AnyString
    bool positive = true;                 // eq? / match? / any-of? (not the not- forms)
    bool match_all = true;                // eq? / match? (not the any- forms)
};

struct Query {
    TSQuery* query = nullptr;
    std::vector<std::vector<Predicate>> predicates;  // per pattern
    std::vector<std::string> injection_language;    // per pattern, from `#set! injection.language`
    std::vector<std::string> capture_names;          // per capture id
};

// Compile a query and its predicates; empty `error` on success.
Query compile_query(const TSLanguage* language, const char* source, size_t len, std::string& error) {
    Query q;
    uint32_t offset = 0;
    TSQueryError type = TSQueryErrorNone;
    q.query = ts_query_new(language, source, uint32_t(len), &offset, &type);
    if (!q.query) {
        error = "query error " + std::to_string(int(type)) + " at byte " + std::to_string(offset);
        return q;
    }
    for (uint32_t i = 0; i < ts_query_capture_count(q.query); i++) {
        uint32_t n = 0;
        const char* name = ts_query_capture_name_for_id(q.query, i, &n);
        q.capture_names.emplace_back(name, n);
    }
    auto string_value = [&](uint32_t id) {
        uint32_t n = 0;
        const char* s = ts_query_string_value_for_id(q.query, id, &n);
        return std::string(s, n);
    };
    uint32_t patterns = ts_query_pattern_count(q.query);
    q.predicates.resize(patterns);
    q.injection_language.resize(patterns);
    for (uint32_t p = 0; p < patterns; p++) {
        uint32_t count = 0;
        const TSQueryPredicateStep* steps = ts_query_predicates_for_pattern(q.query, p, &count);
        // Steps form predicates separated by Done steps: (op arg arg ...) Done ...
        for (uint32_t s = 0; s < count;) {
            uint32_t e = s;
            while (e < count && steps[e].type != TSQueryPredicateStepTypeDone) e++;
            const TSQueryPredicateStep* a = steps + s;  // a[0] = operator
            size_t n = e - s;
            s = e + 1;
            if (n == 0 || a[0].type != TSQueryPredicateStepTypeString) continue;
            std::string op = string_value(a[0].value_id);
            bool first_is_capture = n >= 2 && a[1].type == TSQueryPredicateStepTypeCapture;
            Predicate pr{};
            if ((op == "eq?" || op == "not-eq?" || op == "any-eq?" || op == "any-not-eq?") && n == 3 && first_is_capture) {
                pr.positive = op == "eq?" || op == "any-eq?";
                pr.match_all = op == "eq?" || op == "not-eq?";
                pr.capture = a[1].value_id;
                if (a[2].type == TSQueryPredicateStepTypeCapture) {
                    pr.kind = Predicate::EqCapture;
                    pr.capture2 = a[2].value_id;
                } else {
                    pr.kind = Predicate::EqString;
                    pr.text = string_value(a[2].value_id);
                }
            } else if ((op == "match?" || op == "not-match?" || op == "any-match?" || op == "any-not-match?") && n == 3 &&
                       first_is_capture && a[2].type == TSQueryPredicateStepTypeString) {
                pr.kind = Predicate::MatchString;
                pr.positive = op == "match?" || op == "any-match?";
                pr.match_all = op == "match?" || op == "not-match?";
                pr.capture = a[1].value_id;
                // The vendored regexes use a subset where ECMAScript and Rust's
                // regex crate agree (checked by grammars/vendor.py).
                try {
                    pr.regex = std::make_shared<std::regex>(string_value(a[2].value_id), std::regex::ECMAScript);
                } catch (const std::regex_error&) {
                    error = "invalid regex in pattern " + std::to_string(p);
                    return q;
                }
            } else if ((op == "any-of?" || op == "not-any-of?") && n >= 2 && first_is_capture) {
                pr.kind = Predicate::AnyString;
                pr.positive = op == "any-of?";
                pr.capture = a[1].value_id;
                for (size_t i = 2; i < n; i++) pr.values.push_back(string_value(a[i].value_id));
            } else {
                // `#set! injection.language "javascript"`; anything else is ignored
                // (like general predicates in the Rust crate).
                if (op == "set!" && n == 3 && a[1].type == TSQueryPredicateStepTypeString &&
                    a[2].type == TSQueryPredicateStepTypeString && string_value(a[1].value_id) == "injection.language")
                    q.injection_language[p] = string_value(a[2].value_id);
                continue;
            }
            q.predicates[p].push_back(std::move(pr));
        }
    }
    return q;
}

// Does a match pass its pattern's text predicates? (Same logic, same quirks, as
// QueryMatch::satisfies_text_predicates in the tree-sitter crate 0.27.)
bool satisfies(const Query& q, const TSQueryMatch& m, std::string_view text) {
    auto nodes_for = [&](uint32_t capture) {
        std::vector<std::string_view> texts;
        for (uint16_t i = 0; i < m.capture_count; i++) {
            if (m.captures[i].index != capture) continue;
            uint32_t s = ts_node_start_byte(m.captures[i].node), e = ts_node_end_byte(m.captures[i].node);
            texts.push_back(text.substr(s, e - s));
        }
        return texts;
    };
    for (const Predicate& p : q.predicates[m.pattern_index]) {
        bool ok = true;
        switch (p.kind) {
        case Predicate::EqCapture: {
            auto a = nodes_for(p.capture), b = nodes_for(p.capture2);
            size_t i = 0;
            bool decided = false;
            for (; i < a.size() && i < b.size(); i++) {
                bool is_positive_match = a[i] == b[i];
                if (is_positive_match != p.positive && p.match_all) { ok = false; decided = true; break; }
                if (is_positive_match == p.positive && !p.match_all) { ok = true; decided = true; break; }
            }
            if (!decided) ok = i == a.size() && i == b.size();
            break;
        }
        case Predicate::EqString:
        case Predicate::MatchString:
            for (std::string_view t : nodes_for(p.capture)) {
                bool is_positive_match = p.kind == Predicate::EqString ? t == p.text
                                                                       : std::regex_search(t.begin(), t.end(), *p.regex);
                if (is_positive_match != p.positive && p.match_all) { ok = false; break; }
                if (is_positive_match == p.positive && !p.match_all) { ok = true; break; }
            }
            break;
        case Predicate::AnyString:
            for (std::string_view t : nodes_for(p.capture)) {
                bool any = std::find(p.values.begin(), p.values.end(), t) != p.values.end();
                if (any != p.positive) { ok = false; break; }
            }
            break;
        }
        if (!ok) return false;
    }
    return true;
}

// --- languages ready to use -------------------------------------------------------

struct LangData {
    const TSLanguage* language = nullptr;
    Query highlights;
    std::vector<std::optional<size_t>> capture_styles;  // theme entry per capture id
    std::optional<Query> injections;
    std::string error;                                   // non-empty: unusable
};

// Compile a language's queries on first use (thread-safe; kept for the app's life).
const LangData& lang_data(size_t index) {
    static std::vector<LangData> data(kEmbeddedGrammarCount);
    static std::vector<std::once_flag> once(kEmbeddedGrammarCount);
    std::call_once(once[index], [index] {
        const EmbeddedGrammar& g = kEmbeddedGrammars[index];
        LangData& d = data[index];
        d.language = g.language();
        std::string err;
        d.highlights = compile_query(d.language, g.highlights, g.highlights_len, err);
        if (!err.empty()) {
            d.error = "highlights.scm: " + err;
            return;
        }
        for (const std::string& name : d.highlights.capture_names) d.capture_styles.push_back(find_syntax_entry(entries(), name));
        if (g.injections_len > 0) {
            d.injections = compile_query(d.language, g.injections, g.injections_len, err);
            if (!err.empty()) d.error = "injections.scm: " + err;
        }
    });
    return data[index];
}

// --- the highlighter -------------------------------------------------------------

struct TreeDeleter {
    void operator()(TSTree* t) const { ts_tree_delete(t); }
};
using TreePtr = std::unique_ptr<TSTree, TreeDeleter>;

// Parse `text`, giving up (null) when the deadline passes.
TreePtr parse(std::string_view text, const LangData& d, Clock::time_point deadline) {
    TSParser* parser = ts_parser_new();
    TreePtr tree;
    if (ts_parser_set_language(parser, d.language)) {
        TSInput input{};
        input.payload = &text;
        input.read = [](void* payload, uint32_t at, TSPoint, uint32_t* bytes_read) -> const char* {
            auto* t = static_cast<std::string_view*>(payload);
            if (at >= t->size()) {
                *bytes_read = 0;
                return "";
            }
            *bytes_read = uint32_t(t->size() - at);
            return t->data() + at;
        };
        input.encoding = TSInputEncodingUTF8;
        TSParseOptions options{};
        options.payload = &deadline;
        options.progress_callback = [](TSParseState* state) {  // true = stop
            return Clock::now() > *static_cast<Clock::time_point*>(state->payload);
        };
        tree.reset(ts_parser_parse_with_options(parser, nullptr, input, options));
    }
    ts_parser_delete(parser);
    return tree;
}

// Runs a query over a whole tree, calling `on_match` for every match that
// passes its predicates. Returns false if the deadline passed.
template <class F>
bool for_each_match(const Query& q, TSTree* tree, std::string_view text, Clock::time_point deadline, F&& on_match) {
    struct Progress {
        Clock::time_point deadline;
        bool timed_out = false;
    } progress{deadline};
    TSQueryCursorOptions options{};
    options.payload = &progress;
    options.progress_callback = [](TSQueryCursorState* state) {
        auto* p = static_cast<Progress*>(state->payload);
        if (Clock::now() > p->deadline) p->timed_out = true;
        return p->timed_out;
    };
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec_with_options(cursor, q.query, ts_tree_root_node(tree), &options);
    TSQueryMatch m;
    while (ts_query_cursor_next_match(cursor, &m))
        if (satisfies(q, m, text)) on_match(m);
    ts_query_cursor_delete(cursor);
    return !progress.timed_out;
}

// One capture that may colour some bytes (step 2).
struct Candidate {
    uint32_t start, end, pattern, capture;
    size_t style;
};

// "Which byte at or after i is still unpainted?" in near-constant time
// (union-find: each painted byte points further right).
struct FreeBytes {
    std::vector<size_t> next;
    explicit FreeBytes(size_t len) : next(len + 1) {
        for (size_t i = 0; i <= len; i++) next[i] = i;
    }
    size_t next_free(size_t i) {
        while (next[i] != i) {
            next[i] = next[next[i]];  // path halving
            i = next[i];
        }
        return i;
    }
    void take(size_t i) { next[i] = i + 1; }
};

// Steps 2-3 for one language over all of `text` (parsed as `tree`): fill
// `styles` (one entry per byte, initially kNoStyle) with theme entry indices.
bool paint(std::string_view text, TSTree* tree, const LangData& d, Clock::time_point deadline, std::vector<size_t>& styles) {
    std::vector<Candidate> candidates;
    bool in_time = for_each_match(d.highlights, tree, text, deadline, [&](const TSQueryMatch& m) {
        for (uint16_t i = 0; i < m.capture_count; i++) {
            const TSQueryCapture& c = m.captures[i];
            uint32_t s = ts_node_start_byte(c.node), e = ts_node_end_byte(c.node);
            if (auto style = d.capture_styles[c.index]; style && s < e)
                candidates.push_back({s, e, m.pattern_index, c.index, *style});
        }
    });
    if (!in_time) return false;

    // Best candidates first (shortest node, then LATEST pattern, latest capture,
    // leftmost); each paints only the bytes still free.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return std::make_tuple(a.end - a.start, b.pattern, b.capture, a.start) <
               std::make_tuple(b.end - b.start, a.pattern, a.capture, b.start);
    });
    FreeBytes free(text.size());
    for (const Candidate& c : candidates) {
        for (size_t i = free.next_free(c.start); i < c.end; i = free.next_free(i + 1)) {
            styles[i] = c.style;
            free.take(i);
        }
    }
    return true;
}

// Step 4: (start, end, language) of every injection, in document order.
bool injections(std::string_view text, TSTree* tree, const Query& q, Clock::time_point deadline,
                std::vector<std::tuple<uint32_t, uint32_t, std::string>>& found) {
    auto content = std::find(q.capture_names.begin(), q.capture_names.end(), "injection.content");
    if (content == q.capture_names.end()) return true;
    uint32_t content_id = uint32_t(content - q.capture_names.begin());
    bool in_time = for_each_match(q, tree, text, deadline, [&](const TSQueryMatch& m) {
        const std::string& language = q.injection_language[m.pattern_index];
        if (language.empty()) return;
        for (uint16_t i = 0; i < m.capture_count; i++) {
            if (m.captures[i].index != content_id) continue;
            uint32_t s = ts_node_start_byte(m.captures[i].node), e = ts_node_end_byte(m.captures[i].node);
            if (s < e) found.emplace_back(s, e, language);
        }
    });
    std::sort(found.begin(), found.end());
    return in_time;
}

// Steps 1-5: (start, end, theme entry index) runs, plain ones included.
bool highlight_styles(std::string_view code, std::string_view lang, std::vector<std::tuple<uint32_t, uint32_t, size_t>>& runs) {
    auto index = find_language(lang);
    if (!index) return false;
    const LangData& d = lang_data(*index);
    if (!d.error.empty() || code.size() > kHighlightMaxBytes) return false;
    const auto deadline = Clock::now() + kTimeBudget;

    TreePtr tree = parse(code, d, deadline);
    if (!tree) return false;
    std::vector<size_t> styles(code.size(), kNoStyle);
    if (!paint(code, tree.get(), d, deadline, styles)) return false;

    // Step 4: injected languages, painted over the host's styles.
    if (d.injections) {
        std::vector<std::tuple<uint32_t, uint32_t, std::string>> found;
        if (!injections(code, tree.get(), *d.injections, deadline, found)) return false;
        for (const auto& [start, end, inner] : found) {
            auto inner_index = find_language(inner);
            if (!inner_index) continue;
            const LangData& inner_data = lang_data(*inner_index);
            if (!inner_data.error.empty()) continue;
            std::string_view inner_text = code.substr(start, end - start);
            TreePtr inner_tree = parse(inner_text, inner_data, deadline);
            if (!inner_tree) return false;
            std::vector<size_t> inner_styles(inner_text.size(), kNoStyle);
            if (!paint(inner_text, inner_tree.get(), inner_data, deadline, inner_styles)) return false;
            for (size_t i = 0; i < inner_styles.size(); i++)
                if (inner_styles[i] != kNoStyle) styles[start + i] = inner_styles[i];
        }
    }

    // Step 5: runs of equal style.
    runs.clear();
    for (size_t i = 0; i < styles.size();) {
        size_t style = styles[i], start = i;
        while (i < styles.size() && styles[i] == style) i++;
        if (style != kNoStyle) runs.emplace_back(uint32_t(start), uint32_t(i), style);
    }
    return true;
}

// Uncached highlight (what highlight_code caches): the non-plain runs.
std::vector<CodeSpan> highlight_uncached(std::string_view code, std::string_view lang) {
    std::vector<CodeSpan> spans;
    std::vector<std::tuple<uint32_t, uint32_t, size_t>> runs;
    if (!highlight_styles(code, lang, runs)) return spans;
    for (const auto& [start, end, entry] : runs)
        if (!entries()[entry].plain) spans.push_back({start, end, uint32_t(entry)});
    return spans;
}

}  // namespace

const std::vector<CodeSpan>& highlight_code(const std::string& code, const std::string& lang) {
    static std::map<std::pair<std::string, std::string>, std::vector<CodeSpan>> cache;
    auto key = std::make_pair(code, lang);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    if (cache.size() > 512) cache.clear();
    return cache[key] = highlight_uncached(code, lang);
}

std::vector<std::string> highlight_entry_names() {
    std::vector<std::string> names;
    for (const SyntaxEntry& e : entries()) names.push_back(e.name);
    return names;
}

void highlight_warm_up() {
    std::thread([] {
        if (auto index = find_language("rust")) lang_data(*index);
    }).detach();
}

bool highlight_named(std::string_view code, std::string_view lang, std::vector<NamedSpan>& out) {
    out.clear();
    std::vector<std::tuple<uint32_t, uint32_t, size_t>> runs;
    if (!highlight_styles(code, lang, runs)) return false;
    for (const auto& [start, end, entry] : runs)
        if (!entries()[entry].plain) out.push_back({start, end, entries()[entry].name});
    return true;
}

std::string highlight_golden_text(std::string_view code, const std::vector<NamedSpan>& spans) {
    std::string out;
    for (const NamedSpan& s : spans) {
        out += std::to_string(s.start) + " " + std::to_string(s.end) + " " + s.style + " \"";
        for (char c : code.substr(s.start, s.end - s.start)) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
            }
        }
        out += "\"\n";
    }
    return out;
}

std::vector<std::string> highlight_language_names() {
    std::vector<std::string> names;
    for (size_t i = 0; i < kEmbeddedGrammarCount; i++) names.emplace_back(kEmbeddedGrammars[i].name);
    return names;
}

std::vector<std::string> highlight_check_all_languages() {
    std::vector<std::string> errors;
    for (size_t i = 0; i < kEmbeddedGrammarCount; i++)
        if (const LangData& d = lang_data(i); !d.error.empty()) errors.push_back(std::string(kEmbeddedGrammars[i].name) + ": " + d.error);
    return errors;
}
