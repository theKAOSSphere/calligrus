; Vendored for calligrus by vendor.py from: tree-sitter-html/queries/highlights.scm
; Precedence: for the same node, the LAST matching pattern wins.

(tag_name) @tag
(erroneous_end_tag_name) @tag.error
(doctype) @constant
(attribute_name) @attribute
(attribute_value) @string
(comment) @comment

[
  "<"
  ">"
  "</"
  "/>"
] @punctuation.bracket
