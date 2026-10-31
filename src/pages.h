// Where the pages of a document export end (spec 29; pages.rs), without a window so it can be tested.
//
// A pure function on measurements: the app measures every top-level block (its kind, where it
// starts, how tall it is, and where its lines or rows end), and `page_ends` says at which y each
// page ends. All y values are document coordinates (points, y down).
#pragma once

#include <string>
#include <vector>

namespace pages {

// Two y values closer than this (points) count as equal.
constexpr float kEps = 0.5f;

// The margin on every side of a page (points: 3/4 inch).
constexpr float kMargin = 54.0f;

// The page number "n / N": its size (points), and how far above the page's bottom edge the bottom
// of its text box sits (the baseline is about 2 pt higher: 27 pt above the edge).
constexpr float kNumberSize = 9.0f;
constexpr float kNumberBoxBottom = 25.0f;

// The size (width, height in points) of a paper called `name` ("a4" or "letter", any case).
bool paper_size(const std::string& name, float& width, float& height);

// The range of `--body-size`, in points.
constexpr float kBodyPtMin = 8.0f, kBodyPtMax = 64.0f;

// Points per CSS pixel (1 px = 0.75 pt): `body_size` is in logical pixels on screen.
constexpr double kPtPerPx = 0.75;

// The body size a pages export gets from the app: the config's body size (logical pixels) times the
// zoom (the size the user sees), converted to points, rounded to half a point and kept in the range
// above. (The CLI default, without `--body-size`, is this at zoom 1.)
float export_body_size(int body_size, double zoom);

// What a top-level block is, as far as page breaks care.
enum class Kind {
    Text,     // paragraph, footnote, list, quote, callout: breaks between lines, widow/orphan rule per paragraph/item
    Heading,  // text too, but a page never ends right after it
    Code,     // breaks between code lines, widow/orphan rule on its lines
    Table,    // breaks between rows
    Atomic,   // a row of pictures, display maths, a Mermaid diagram, a rule: never broken
};

// One top-level block, measured.
struct Block {
    Kind kind = Kind::Text;
    // Where the block starts, and how tall it is.
    float top = 0;
    float height = 0;
    // The bottoms of its line boxes, one list per paragraph / list item / quote child (Text and
    // Heading). A code block has one list (its code lines), a table one list (its rows); an Atomic
    // block has none. The last bottom of the last list is the block's end.
    std::vector<std::vector<float>> groups;
};

// Is a break after the k-th of n lines of one paragraph / item allowed by the widow/orphan rule?
// In a paragraph of 4+ lines, neither the first 2 nor the last 2 lines may be alone on a page: so
// k >= 2 and n - k >= 2. Shorter paragraphs may break anywhere. k == n is the place between two
// paragraphs or items, always fine.
bool widow_ok(size_t k, size_t n);

// The places inside one block where a page may end: between its lines (or code lines, rows),
// except after the very last one (that is the block's end; the next block's top covers it).
// `strict` applies the widow/orphan rule (Text, Heading and Code; table rows are exempt). Appends to `out`.
void inner_places(const Block& b, bool strict, std::vector<float>& out);

// Every place where a page may end (in block order, not sorted). Between blocks: the top of block
// i (i > 0), but not right after a heading. `strict` is the rule set of the first try; with it
// off, only the heading and widow/orphan rules are dropped (an atomic block never breaks inside).
std::vector<float> allowed_places(const std::vector<Block>& blocks, bool strict);

// The largest place in (y0, limit], with kEps of slack: a place at y0 would make an empty page,
// one a hair past the limit still fits. Returns false if there is none.
bool largest_in(const std::vector<float>& places, float y0, float limit, float& found);

// Where page after page ends, for a text area `h` tall and a document `total` tall: the last end
// is `total`. Starting at y0 (0 for the first page), a page ends at the largest allowed place in
// (y0, y0 + h]. If that place is above y0 + h/4 (or there is none), it ends at the largest place
// ignoring the heading and widow/orphan rules; if there is still none, at y0 + h (the tall thing
// is cut). The next page starts where this one ended.
std::vector<float> page_ends(const std::vector<Block>& blocks, float h, float total);

}  // namespace pages
