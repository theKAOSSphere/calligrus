// Org-mode parser (spec 25): a line-based parser that produces the same Blocks and line
// targets as the Markdown parser (model.h), so the renderer, outline, find, sync and themes
// need no changes.
#pragma once

#include <string_view>

#include "model.h"

namespace md {

// Parse Org text (UTF-8) into blocks.
Document parse_org(std::string_view src);

// Is `path` an Org file (extension `.org`, any case)?
bool is_org_path(std::string_view path);

// Parse by format: "org" -> Org, anything else -> Markdown.
Document parse_format(std::string_view src, std::string_view format);

}  // namespace md
