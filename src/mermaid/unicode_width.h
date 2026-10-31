// Mermaid port of mermaid-rs-renderer's unicode_width.rs: classification of CJK / emoji code
// points so text widths agree between the glyph-advance path and the fallback path.
#pragma once

#include <optional>
#include <vector>

namespace mermaid {

enum class Cluster { Wide, ZeroWidth };

inline bool is_cjk_wide_char(char32_t c) {
    return (c >= 0x1100 && c <= 0x11ff) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xa960 && c <= 0xa97f) ||
           (c >= 0xac00 && c <= 0xd7ff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe1f) ||
           (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff01 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
           (c >= 0x20000 && c <= 0x2fa1f) || (c >= 0x30000 && c <= 0x323af);
}
inline bool is_emoji_wide_char(char32_t c) { return (c >= 0x2600 && c <= 0x27bf) || (c >= 0x1f000 && c <= 0x1faff); }
inline bool is_emoji_modifier_char(char32_t c) {
    return c == 0x200d || c == 0x20e3 || (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0x1f3fb && c <= 0x1f3ff);
}
inline bool is_regional_indicator(char32_t c) { return c >= 0x1f1e6 && c <= 0x1f1ff; }
inline bool is_keycap_starter(char32_t c) { return (c >= '0' && c <= '9') || c == '#' || c == '*'; }

struct ClusterStep {
    Cluster kind;
    size_t next;  // index just past the cluster
};

// Try to consume a multi-code-point cluster starting at chars[idx] (keycap, flag, emoji with
// joiners/modifiers, lone modifier). nullopt for plain characters, CJK included.
inline std::optional<ClusterStep> consume_cluster(const std::vector<char32_t>& chars, size_t idx) {
    if (idx >= chars.size()) return std::nullopt;
    char32_t ch = chars[idx];
    auto at = [&](size_t i) -> char32_t { return i < chars.size() ? chars[i] : 0xFFFFFFFFu; };

    if (is_keycap_starter(ch)) {
        size_t next = idx + 1;
        size_t keycap = (at(next) == 0xfe0f) ? next + 1 : next;
        if (at(keycap) == 0x20e3) return ClusterStep{Cluster::Wide, keycap + 1};
    }
    if (is_regional_indicator(ch) && idx + 1 < chars.size() && is_regional_indicator(chars[idx + 1]))
        return ClusterStep{Cluster::Wide, idx + 2};
    if (is_emoji_wide_char(ch)) {
        size_t end = idx + 1;
        while (end < chars.size()) {
            if (chars[end] == 0x200d && end + 1 < chars.size() && is_emoji_wide_char(chars[end + 1])) end += 2;
            else if (is_emoji_modifier_char(chars[end])) end += 1;
            else break;
        }
        return ClusterStep{Cluster::Wide, end};
    }
    if (is_emoji_modifier_char(ch)) return ClusterStep{Cluster::ZeroWidth, idx + 1};
    return std::nullopt;
}

}  // namespace mermaid
