// The document model: what a Markdown file looks like after parsing.
//
// Like the Rust version (the Rust port) we do NOT draw straight from
// the parser's callbacks. We first build a small tree of Blocks and Spans and
// the renderer walks that tree. Parsing and drawing stay independent.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace md {

// A run of text that shares one style. A paragraph is a list of these.
struct Span {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    bool math = false;     // TeX source (rendered by the Rust math engine)
    bool small = false;    // footnote reference `[n]`: drawn at 0.8x size
    bool display = false;  // math from $$..$$ (lifted into a Block::Kind::Math by parse())
    std::string link;   // non-empty => this span is a link to that URL

    // Image (`![alt](src)` or `<img>`): `text` holds (part of) the alt text.
    std::string image_src;    // non-empty => this span belongs to an image
    float image_width = 0;    // `<img width="...">`, 0 = natural size
    bool image_start = false; // first span of each image (alt text may follow in more spans)
};

// One picture in a Block::Kind::Images row.
struct ImageRef {
    std::string src, alt;
    float width = 0;   // 0 = natural size
    std::string link;  // where a click goes (`[![alt](img)](url)`), may be empty
};

using Spans = std::vector<Span>;

enum class Align { Default, Left, Center, Right };

// One block-level element. Rust uses an enum with per-variant data; here we use
// one struct with a `kind` tag and only fill the fields that kind needs. It is
// less type-safe than std::variant but much easier to read and to build up
// incrementally from md4c's callbacks.
struct Block {
    // Math: `text` = TeX of a $$..$$ display. Images: a paragraph holding only pictures.
    enum class Kind { Heading, Paragraph, Code, Quote, List, Table, Rule, Math, Images };
    Kind kind = Kind::Paragraph;

    int level = 0;                          // Heading: 1..6
    Spans spans;                            // Heading, Paragraph

    std::string lang, text;                 // Code: info string + body

    std::vector<Block> children;            // Quote: nested blocks
    std::string callout;                    // Quote: "note"/"tip"/"important"/"warning"/"caution" (GitHub callout), else empty
    int footnote = 0;                       // Paragraph: this is footnote n's text in the end section (anchor #fn-n); 0 = no

    bool ordered = false;                   // List
    unsigned start = 1;                     // List: first number of an ordered list
    std::vector<std::vector<Block>> items;  // List: each item is a list of blocks

    std::vector<Align> aligns;              // Table: one per column
    std::vector<Spans> header;              // Table: header cells
    std::vector<std::vector<Spans>> rows;   // Table: body rows of cells

    std::vector<ImageRef> images;           // Images: laid out as a wrapping row

    // The lines of the ORIGINAL text (1-based, inclusive) this block came from, for
    // editor scroll sync. Set for top-level blocks and for the children of a top-level
    // Quote (but not deeper). 0 = unknown.
    int line_first = 0, line_last = 0;
    // Top-level List: the same for each item (parallel to `items`).
    struct Lines {
        int first = 0, last = 0;
    };
    std::vector<Lines> item_lines;
    // Callout: the `> [!NOTE]` line (its title); 0 = unknown. (The marker paragraph itself is dropped.)
    int title_line = 0;
};

using Document = std::vector<Block>;

// Preprocessing steps of parse(), public so the tests can reach them.
// Remove a leading YAML front matter block (`---` .. `---` or `...`).
std::string strip_front_matter(std::string_view src);
// Pull `[^label]: text` definitions out of `src` (outside fences) and turn each
// `[^label]` reference into a link `[\[n\]](#fn-n)`, n in order of first reference.
// notes[n-1] is the text of footnote n. Unknown labels stay literal.
// `line_map` (optional): for each line of the result, the index of the input line it came from.
std::string extract_footnotes(std::string_view src, std::vector<std::string>& notes, std::vector<int>* line_map = nullptr);
// "[!NOTE]" (case-insensitive, trailing spaces ok) -> "note"; anything else -> "".
std::string callout_kind(std::string_view line);

// Lift `display` math spans and picture-only paragraphs into their own blocks (used by the Org parser too).
void lift_display_blocks(std::vector<Block>& blocks);

// Parse Markdown (UTF-8) into a list of blocks. Uses md4c with GitHub flavour
// (tables, strikethrough, task lists) plus $...$ math spans.
Document parse(std::string_view src);

// Is `bytes` (a file's content) really a binary file? A known signature (PDF, PNG, JPEG, GIF, ZIP),
// a NUL byte in the first 8 KB, or more than 30 % invalid UTF-8 there. Returns what it looks like
// ("a PDF", "binary data"...), or nullptr for text. (Same rules as the Rust port)
const char* looks_binary(std::string_view bytes);
// The note shown instead of such a file.
std::string binary_message(const std::string& name, const std::string& kind);

// The largest file Calligrus lays out (spec 3: very large files). Laying text out costs hundreds of times the file's size in memory
// (a 7.8 MB CSV took 3 GB on a tablet and the system killed the app), so a bigger file gets a note instead of its text.
// Android: a hard limit of 1 MB (decimal, as file managers show sizes). Desktop: a soft limit of 50 MB, the note offers "Open anyway".
constexpr size_t kMaxFileBytesAndroid = 1000000;
constexpr size_t kMaxFileBytesDesktop = 50000000;
constexpr size_t max_file_bytes(bool android) { return android ? kMaxFileBytesAndroid : kMaxFileBytesDesktop; }
// The note shown instead of a file of `bytes` bytes that is over the limit. Hard (Android): "name is too large to display: 7.8 MB,
// the limit is 1.0 MB."; soft (desktop): "name is 63.2 MB and may take a long time and a lot of memory to display."
std::string too_large_message(const std::string& name, uint64_t bytes, bool android);
// Files that are data, not documents (.csv, .tsv, .log): shown as plain text, line by line, not parsed as Markdown (whose single
// newlines would join all the rows into one paragraph).
bool is_plain_text_path(std::string_view path);
// The Markdown that shows `text` as it is: one fenced code block (the fence is longer than any run of backquotes in the text).
std::string plain_text_as_markdown(std::string_view text);


}  // namespace md
