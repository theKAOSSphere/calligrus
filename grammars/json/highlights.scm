; Vendored for calligrus by vendor.py from: tree-sitter-json/queries/highlights.scm
; Precedence: for the same node, the LAST matching pattern wins.

(pair
  key: (_) @string.special.key)

(string) @string

(number) @number

[
  (null)
  (true)
  (false)
] @constant.builtin

(escape_sequence) @escape

(comment) @comment
