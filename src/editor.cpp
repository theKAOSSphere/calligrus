#include "editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>

namespace editor {
namespace {

// A flat JSON object: member name -> string or number. Other values (true,
// null, arrays, nested objects) are skipped over.
struct Value {
    bool is_string = false, is_number = false, is_bool = false, flag = false;
    std::string str;
    double num = 0;
};

class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}
    std::string error;

    bool object(std::map<std::string, Value>& out) {
        ws();
        if (!eat('{')) return fail("not a JSON object");
        ws();
        if (eat('}')) return true;
        for (;;) {
            ws();
            std::string key;
            if (!string(key)) return fail("bad member name");
            ws();
            if (!eat(':')) return fail("expected ':'");
            Value v;
            if (!value(v)) return false;
            out[key] = std::move(v);
            ws();
            if (eat(',')) continue;
            if (eat('}')) break;
            return fail("expected ',' or '}'");
        }
        ws();
        return pos_ == s_.size() ? true : fail("trailing characters");
    }

private:
    std::string_view s_;
    size_t pos_ = 0;

    bool fail(const char* m) {
        if (error.empty()) error = m;
        return false;
    }
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) pos_++;
    }
    bool eat(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) return pos_++, true;
        return false;
    }
    static void put_utf8(std::string& o, char32_t cp) {
        if (cp < 0x80) {
            o += char(cp);
        } else if (cp < 0x800) {
            o += char(0xC0 | (cp >> 6));
            o += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += char(0xE0 | (cp >> 12));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        } else {
            o += char(0xF0 | (cp >> 18));
            o += char(0x80 | ((cp >> 12) & 0x3F));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        }
    }
    std::optional<unsigned> hex4() {
        if (pos_ + 4 > s_.size()) return std::nullopt;
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return std::nullopt;
        }
        return v;
    }
    bool string(std::string& out) {
        if (!eat('"')) return false;
        for (;;) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return fail("unterminated string");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    auto hi = hex4();
                    if (!hi) return fail("bad \\u escape");
                    char32_t cp = *hi;
                    if (cp >= 0xD800 && cp < 0xDC00) {  // high surrogate: a low one (\uDC00..\uDFFF) must follow
                        cp = 0xFFFD;
                        if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            const size_t save = pos_;
                            pos_ += 2;
                            auto lo = hex4();
                            if (lo && *lo >= 0xDC00 && *lo < 0xE000) cp = 0x10000 + ((*hi - 0xD800) << 10) + (*lo - 0xDC00);
                            else pos_ = save;
                        }
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        cp = 0xFFFD;  // lone low surrogate
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }
    bool skip_value() {  // any JSON value we don't need
        ws();
        if (pos_ >= s_.size()) return fail("unexpected end");
        const char c = s_[pos_];
        if (c == '{' || c == '[') {
            const char open = c, close = c == '{' ? '}' : ']';
            int depth = 0;
            while (pos_ < s_.size()) {
                const char d = s_[pos_];
                if (d == '"') {
                    std::string ignored;
                    if (!string(ignored)) return false;
                    continue;
                }
                pos_++;
                if (d == open) depth++;
                else if (d == close && --depth == 0) return true;
            }
            return fail("unterminated value");
        }
        const size_t start = pos_;  // true / false / null
        while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' && s_[pos_] != ' ' && s_[pos_] != '\t' && s_[pos_] != '\r') pos_++;
        return pos_ > start ? true : fail("bad value");
    }
    bool value(Value& v) {
        ws();
        if (pos_ < s_.size() && s_[pos_] == '"') {
            v.is_string = true;
            return string(v.str);
        }
        if (pos_ < s_.size() && (s_[pos_] == '-' || (s_[pos_] >= '0' && s_[pos_] <= '9'))) {
            const std::string rest(s_.substr(pos_, 40));
            char* end = nullptr;
            v.num = std::strtod(rest.c_str(), &end);
            if (end == rest.c_str()) return fail("bad number");
            pos_ += size_t(end - rest.c_str());
            v.is_number = true;
            return true;
        }
        for (const char* word : {"true", "false"})
            if (s_.compare(pos_, std::strlen(word), word) == 0) {
                v.is_bool = true;
                v.flag = word[0] == 't';
                pos_ += std::strlen(word);
                return true;
            }
        return skip_value();
    }
};

}  // namespace

bool parse_message(std::string_view line, Message& out, std::string& error) {
    error.clear();
    std::map<std::string, Value> f;
    Reader r(line);
    if (!r.object(f)) {
        error = r.error.empty() ? "malformed JSON" : r.error;
        return false;
    }
    auto it = f.find("type");
    if (it == f.end() || !it->second.is_string) {
        error = "missing \"type\"";
        return false;
    }
    const std::string& t = it->second.str;
    auto str = [&](const char* name, std::string& dst, bool required) {
        auto i = f.find(name);
        if (i == f.end() || !i->second.is_string) {
            if (required) error = std::string("missing string field \"") + name + "\"";
            return !required;
        }
        dst = i->second.str;
        return true;
    };
    Message m;
    if (t == "open") {
        m.type = Message::Type::Open;
        if (!str("doc", m.doc, true) || !str("path", m.path, false) || !str("text", m.text, true) || !str("format", m.format, false)) return false;
    } else if (t == "text") {
        m.type = Message::Type::Text;
        if (!str("doc", m.doc, true) || !str("text", m.text, true)) return false;
    } else if (t == "cursor") {
        m.type = Message::Type::Cursor;
        if (!str("doc", m.doc, true)) return false;
        auto i = f.find("line");
        if (i == f.end() || !i->second.is_number) {
            error = "missing number field \"line\"";
            return false;
        }
        m.line = int(std::lround(i->second.num));
    } else if (t == "close") {
        m.type = Message::Type::Close;
        if (!str("doc", m.doc, true)) return false;
    } else if (t == "search") {
        m.type = Message::Type::Search;
        if (!str("doc", m.doc, true) || !str("text", m.text, true)) return false;
        auto i = f.find("case");
        m.case_sensitive = i != f.end() && i->second.is_bool && i->second.flag;
    } else if (t == "quit") {
        m.type = Message::Type::Quit;
    } else {
        return false;  // unknown type: ignored (no error)
    }
    out = std::move(m);
    return true;
}

bool supersedes(const Message& newer, const Message& older) {
    return newer.type == Message::Type::Text && older.type == Message::Type::Text && newer.doc == older.doc;
}

size_t target_block(const std::vector<LineRange>& blocks, int line) {
    size_t idx = 0;
    for (size_t i = 0; i < blocks.size(); i++) {
        if (blocks[i].first <= 0 || blocks[i].first > line) continue;
        idx = i;                           // the last block starting before the line...
        if (line <= blocks[i].last) break;  // ...unless one contains it
    }
    return idx;
}

SyncTarget select_target(const std::vector<SyncNode>& blocks, int line) {
    SyncTarget t;
    if (blocks.empty()) return t;
    std::vector<LineRange> ranges;
    ranges.reserve(blocks.size());
    for (const SyncNode& b : blocks) ranges.push_back(b.lines);
    t.block = target_block(ranges, line);
    const SyncNode& top = blocks[t.block];
    t.lines = top.lines;
    bool plain = top.plain;
    if (!top.subs.empty()) {
        std::vector<LineRange> subs;
        subs.reserve(top.subs.size());
        for (const SyncNode& s : top.subs) subs.push_back(s.lines);
        const size_t k = target_block(subs, line);
        if (subs[k].first > 0) {
            t.sub = int(k);
            t.lines = subs[k];
            plain = top.subs[k].plain;
        }
    }
    t.tick = t.lines.first > 0 && t.lines.last > t.lines.first && !plain;
    return t;
}

float line_fraction(const LineRange& b, int line) {
    if (b.first <= 0 || b.last < b.first) return 0;
    // Like the Rust port (editor.rs locate_line): a line past the target's last one (a blank line
    // after it) counts as just below it, so the fraction runs up to 1; a line before it as 0.
    const float f = float(line - b.first) / float(b.last - b.first + 1);
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

namespace {
// The index of the last band that starts at or above y (0 when y is above all of them).
size_t band_at(const std::vector<Band>& bands, float y) {
    size_t idx = 0;
    for (size_t i = 0; i < bands.size(); i++)
        if (bands[i].top <= y) idx = i;
    return idx;
}
}  // namespace

int line_at(const std::vector<SyncNode>& blocks, float y, const SpanFn& span, const RowsFn& rows) {
    // 1. The target (see the header). A target that was not measured is skipped.
    int block = -1;
    for (size_t i = 0; i < blocks.size(); i++) {
        const auto s = span(i, -1);
        if (!s) continue;
        if (block < 0 || s->first <= y) block = int(i);
    }
    if (block < 0) return 0;
    const SyncNode& top = blocks[size_t(block)];
    int sub = -1;
    for (size_t k = 0; k < top.subs.size(); k++) {
        const auto s = span(size_t(block), int(k));
        if (!s) continue;
        if (sub < 0 || s->first <= y) sub = int(k);
    }
    const SyncNode& node = sub >= 0 ? top.subs[size_t(sub)] : top;
    const auto measured = span(size_t(block), sub);
    if (!measured) return 0;
    const int first = node.lines.first, last = node.lines.last;
    if (first <= 0) return 0;

    // 2. The line inside it.
    int line = first;
    switch (node.kind) {
        case NodeKind::Code: {
            const std::vector<Band> bands = rows(size_t(block), sub);
            if (!bands.empty()) {
                // The fence line comes first; an indented block has exactly as many lines as rows.
                const int lead = last + 1 - first > int(bands.size()) ? 1 : 0;
                line = first + lead + int(band_at(bands, y));
            }
            break;
        }
        case NodeKind::Table: {
            const std::vector<Band> bands = rows(size_t(block), sub);
            const size_t n = band_at(bands, y);
            line = n == 0 ? first : first + 1 + int(n);  // body row k skips the delimiter row
            break;
        }
        case NodeKind::Atom: break;
        case NodeKind::Text: {
            const float lines = float(std::max(last + 1 - first, 1));
            const float frac = measured->second > 0 ? std::clamp((y - measured->first) / measured->second, 0.0f, 1.0f) : 0.0f;
            // (the 1e-4 only guards against 2.9999998 when y came from a marker position)
            line = first + int(std::floor(frac * lines + 1e-4f));
            break;
        }
    }
    return std::clamp(line, first, std::max(last, first));
}

// `s` as a JSON string, quotes included.
static std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out + "\"";
}

std::string goto_message(std::string_view doc, int line) {
    return R"({"type":"goto","doc":)" + json_string(doc) + R"(,"line":)" + std::to_string(line) + "}";
}

std::string notes_message(std::string_view doc, size_t slide, size_t total, std::string_view notes, std::pair<int, int> lines) {
    return R"({"type":"notes","doc":)" + json_string(doc) + R"(,"slide":)" + std::to_string(slide) + R"(,"total":)" +
           std::to_string(total) + R"(,"notes":)" + json_string(notes) + R"(,"line":)" + std::to_string(lines.first) +
           R"(,"end":)" + std::to_string(lines.second) + "}";
}

bool valid_json_object(std::string_view s) {
    std::map<std::string, Value> m;
    return Reader(s).object(m);
}

bool json_objects_equal(std::string_view a, std::string_view b) {
    std::map<std::string, Value> ma, mb;
    if (!Reader(a).object(ma) || !Reader(b).object(mb) || ma.size() != mb.size()) return false;
    for (const auto& [key, va] : ma) {
        const auto it = mb.find(key);
        if (it == mb.end()) return false;
        const Value& vb = it->second;
        if (va.is_string != vb.is_string || va.is_number != vb.is_number || va.is_bool != vb.is_bool) return false;
        if (va.is_string && va.str != vb.str) return false;
        if (va.is_number && va.num != vb.num) return false;
        if (va.is_bool && va.flag != vb.flag) return false;
    }
    return true;
}

int count_lines(std::string_view text) {
    if (text.empty()) return 0;
    int n = 1;
    for (size_t i = 0; i + 1 < text.size(); i++)
        if (text[i] == '\n') n++;
    return n;
}

}  // namespace editor
