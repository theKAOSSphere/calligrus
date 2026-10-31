// System fonts for missing characters (spec 30; mirrors the Rust port).
//
// When a document has characters that no font of the app's chains has (Gujarati, Thai, CJK...),
// missing_chars() says which they are and pick_system_fonts() which installed fonts to add behind
// the bundled ones. The app adds them to FontFiles::system and rebuilds the atlas (fonts.h).
// No ImGui here, so the tests run in a console.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "font_index.h"  // FontFace
#include "fonts.h"       // FontSource
#include "model.h"

namespace sysfont {

// At most this many fonts are chosen at once, and this many in a whole session.
constexpr size_t kMaxPerPick = 4;
constexpr size_t kMaxPerSession = 8;

// Families tried first, in this order (ASCII case-insensitive; a trailing '*' means "any family
// starting with this", those then go by name). The rest of the installed families come after.
const std::vector<std::string>& preferred_families();

// Characters that may be missing from every font without anyone noticing: spaces, controls,
// joiners and variation selectors (they draw nothing).
bool ignorable(char32_t c);

// The characters of `chars` (each once, sorted) that are not ignorable and that `has` says no font has.
std::vector<char32_t> missing_chars(const std::set<char32_t>& chars, const std::function<bool(char32_t)>& has);

// Does a `cmap` table map `c` to a glyph? (Unicode subtables of format 4, 6, 12 and 13.)
bool cmap_has(std::string_view cmap, char32_t c);

// Every character of the document's text: prose, headings, tables, code; not maths.
std::set<char32_t> document_chars(const md::Document& doc);

// The regular faces of the index in the order they are tried: preferred families first (in the
// order of `preferred`), then everything else; within a group by lower-cased family name.
std::vector<FontFace> ordered_candidates(const std::vector<FontFace>& faces, const std::vector<std::string>& preferred);

// Greedy choice: covers[i] is what candidate i has of `missing`. Repeatedly take the candidate that
// covers the most still-missing characters (ties: the earlier one), until `max` are taken or nothing
// more is covered. Returns the candidates' numbers in the order taken.
std::vector<size_t> choose(const std::vector<std::vector<char32_t>>& covers, const std::vector<char32_t>& missing,
                           size_t max);

// What a candidate has of the missing characters; false: unusable (a colour font, a broken file).
using CoverageFn = std::function<bool(const FontFace&, const std::vector<char32_t>& missing, std::vector<char32_t>& covered)>;

// The installed faces to add for `missing` characters: at most `max`.
std::vector<FontSource> pick_with(const std::vector<FontFace>& faces, const std::vector<std::string>& preferred,
                                  const std::vector<char32_t>& missing, size_t max, const CoverageFn& coverage);

// pick_with on the real font files: reads each candidate's cmap table.
std::vector<FontSource> pick_system_fonts(const std::vector<FontFace>& faces, const std::vector<char32_t>& missing,
                                          size_t max);

}  // namespace sysfont
