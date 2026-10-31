// Editor live preview (spec section 15): the protocol messages the editor
// sends on stdin (one JSON object per line) and their parser. No window or
// ImGui in here, so it can be unit-tested (editor_test.cpp); main.cpp applies them.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace editor {

struct Message {
    enum class Type { Open, Text, Cursor, Close, Quit, Search };
    Type type = Type::Quit;
    std::string doc;   // Open, Text, Cursor, Close
    std::string path;  // Open
    std::string format;  // Open: "markdown" | "org" ("" = by the path's extension)
    std::string text;  // Open, Text
    int line = 0;      // Cursor (1-based)
    bool case_sensitive = false;  // Search (text = the query, "" clears)
};

// Parse one protocol line. Returns false and sets `error` for malformed JSON or a
// missing / mistyped required field; returns false with an EMPTY `error` for an
// unknown `type` (ignored by the caller). Unknown fields are skipped.
bool parse_message(std::string_view line, Message& out, std::string& error);

// Does `newer` make `older` pointless (both Text for the same doc)? Used to coalesce.
bool supersedes(const Message& newer, const Message& older);

// Scroll sync / cursor marker / sync-dump (spec 15, 16): which top-level block a source
// line belongs to. One implementation for all three, so they can never disagree.
struct LineRange {
    int first = 0, last = 0;  // 1-based source lines; first <= 0: the block has no known position
};
// The block containing `line`, else the last block starting before it (0 if none).
size_t target_block(const std::vector<LineRange>& blocks, int line);

// One level into containers (spec 18): a top-level block with, for a list, its items and, for a
// quote / callout, its children (a callout's title first). Everything else has no `subs`.
// How a click inside a target becomes a source line (spec 23, the reverse mapping).
enum class NodeKind {
    Text,   // paragraph, heading, list item, quote child, callout title: interpolate by height
    Code,   // a code block (not Mermaid): the visual code line under the click
    Table,  // a table: the row under the click
    Atom,   // display math, pictures, Mermaid, rules: always the first line
};
struct SyncNode {
    LineRange lines;
    bool plain = false;  // a single paragraph / heading / list item: its lines don't map linearly, so no tick
    std::vector<SyncNode> subs;
    NodeKind kind = NodeKind::Text;
};
// What the editor's cursor line points at.
struct SyncTarget {
    size_t block = 0;   // top-level block
    int sub = -1;       // item / child within it, -1 = the block itself
    LineRange lines;    // the target's source lines
    bool tick = false;  // draw the line tick: more than one source line and not `plain`
};
// The block containing `line` (target_block), then, if it has subs with known lines, the sub
// containing it (a line between subs, e.g. a blank one, belongs to the previous sub).
SyncTarget select_target(const std::vector<SyncNode>& blocks, int line);
// How far down the block `line` is: (line - first) / (last - first + 1), 0 without a range.
float line_fraction(const LineRange& block, int line);
// ---- Two-way sync (spec 23): Ctrl+click in the preview -> a source line --------------------
// The row bands (top, height) of a code block's lines / a table's rows, in document pixels.
struct Band {
    float top = 0, height = 0;
};
// (top, height) of a target in document pixels; `sub` -1 = the top-level block. nullopt = not measured.
using SpanFn = std::function<std::optional<std::pair<float, float>>(size_t block, int sub)>;
// The row bands of a code / table target (empty for others).
using RowsFn = std::function<std::vector<Band>(size_t block, int sub)>;
// The 1-based source line under document height `y`: the exact inverse of select_target +
// line_fraction. Target = the last block starting at or above y (above the first: the first), then
// the same rule among its subs. Code: fence line + 1 + the code line under y (clamped; none when
// the line count says there is no fence line); table: header -> first line, body row k -> first + 2 + k;
// atoms: first line; text: first + floor(fraction * lines). 0 when nothing is measured.
int line_at(const std::vector<SyncNode>& blocks, float y, const SpanFn& span, const RowsFn& rows);
// `{"type":"goto","doc":<doc>,"line":N}` (no newline).
std::string goto_message(std::string_view doc, int line);

// The `notes` message for the editor (spec 26): slides mode is on slide `slide` (1-based) of
// `total`, whose speaker notes are `notes` (maybe empty). One line, no trailing newline.
std::string notes_message(std::string_view doc, size_t slide, size_t total, std::string_view notes, std::pair<int, int> lines);
// Do these two JSON objects hold the same members (compared parsed: order and spacing free)?
// Values: strings, numbers and booleans are compared; any other value counts as equal.
bool json_objects_equal(std::string_view a, std::string_view b);
// Is `s` one JSON object (flat reader as above)?
bool valid_json_object(std::string_view s);

// Number of source lines of `text` (a final newline does not start another line; empty = 0).
int count_lines(std::string_view text);

}  // namespace editor
