# Vendored tree-sitter grammars

These grammars are compiled into the executable by `cmake/highlight.cmake`.
Nothing needs tree-sitter-cli, and no file is read at runtime: everything is compiled or embedded into the exe.

| File | Purpose |
|---|---|
| `languages.txt` | Compiled-in languages and their fence aliases. CMake reads it. |
| `theme.txt` | Capture name → dark colour, light colour, bold/italic. Embedded in the exe. |
| `<lang>/src/` | `parser.c`, maybe `scanner.c`, and the `tree_sitter/*.h` headers the grammar was generated with. |
| `<lang>/highlights.scm` | Final, already-merged highlight query. |
| `html/injections.scm` | `<script>` → JavaScript, `<style>` → CSS. |
| `runtime/` | tree-sitter C runtime (see below). |
| `tests/` | Golden samples and expected spans, checked by `calligrus-highlight-test`. |
| `vendor.py` | Rebuilds all of this from upstream checkouts and documents every change made to the sources. |

## Grammars

| Language | Source | Commit | Licence | Query |
|---|---|---|---|---|
| rust | https://github.com/tree-sitter/tree-sitter-rust | 77a3747266f4d621d0757825e6b11edcbf991ca5 | MIT | upstream |
| c | https://github.com/tree-sitter/tree-sitter-c | b780e47fc780ddc8da13afa35a3f4ed5c157823d | MIT | upstream |
| cpp | https://github.com/tree-sitter/tree-sitter-cpp | c009222808634c1014f82438d4883753516a2c24 | MIT | C query, then C++ query (as in its tree-sitter.json) |
| python | https://github.com/tree-sitter/tree-sitter-python | 26855eabccb19c6abf499fbc5b8dc7cc9ab8bc64 | MIT | upstream |
| javascript | https://github.com/tree-sitter/tree-sitter-javascript | 58404d8cf191d69f2674a8fd507bd5776f46cb11 | MIT | highlights + jsx + params |
| typescript, tsx | https://github.com/tree-sitter/tree-sitter-typescript | 75b3874edb2dc714fb1fd77a32013d0f8699989f | MIT | TS query, then (jsx +) JavaScript query |
| json | https://github.com/tree-sitter/tree-sitter-json | 254c42a6476413b776221e03982ac8ae159eeb72 | MIT | upstream |
| toml | https://github.com/tree-sitter-grammars/tree-sitter-toml | 64b56832c2cffe41758f28e05c756a3a98d16f41 | MIT | upstream |
| yaml | https://github.com/tree-sitter-grammars/tree-sitter-yaml | a1c4812a73ec5e089de8e441fdea3a921e8d5079 | MIT | upstream (core schema) |
| bash | https://github.com/tree-sitter/tree-sitter-bash | a06c2e4415e9bc0346c6b86d401879ffb44058f7 | MIT | upstream |
| powershell | https://github.com/airbus-cert/tree-sitter-powershell | e7bd348c49fdfd5c853a146a670965ba516a6239 | MIT | upstream |
| lua | https://github.com/tree-sitter-grammars/tree-sitter-lua | 10fe0054734eec83049514ea2e718b2a56acd0c9 | MIT | upstream |
| elisp | https://github.com/Wilfred/tree-sitter-elisp | 0cbf0906d9ee707c8c109422fba9cdd17ae13dcf | MIT | upstream |
| markdown | https://github.com/tree-sitter-grammars/tree-sitter-markdown (block grammar only) | a0a00f817d02412bd92c54d316f164d827b57b5c | MIT | upstream |
| latex | https://github.com/latex-lsp/tree-sitter-latex | 8c75e93cd08ccb7ce1ccab22c1fbd6360e3bcea6 | MIT | Helix `runtime/queries/latex/highlights.scm` @ ba40e547426b, **MPL-2.0** (`latex/LICENSE-highlights-helix`) |
| cmake | https://github.com/uyha/tree-sitter-cmake | 58993af75218bc99a1f5a04c832a5937e7c422cb | MIT | upstream |
| go | https://github.com/tree-sitter/tree-sitter-go | 2346a3ab1bb3857b48b29d779a1ef9799a248cd7 | MIT | upstream |
| html | https://github.com/tree-sitter/tree-sitter-html | 73a3947324f6efddf9e17c0ea58d454843590cc0 | MIT | upstream (+ injections) |
| css | https://github.com/tree-sitter/tree-sitter-css | dda5cfc5722c429eaba1c910ca32c2c0c5bb1a3f | MIT | upstream |
| autohotkey (v2) | https://github.com/holy-tao/tree-sitter-autohotkey | d2d7f1f5cbe79b4b04063d663661a8a9e3f7b19e | Unlicense | upstream |

Each grammar's licence is in `<lang>/LICENSE`. Every `parser.c` is the one committed upstream; none were regenerated.
LaTeX is pinned to the revision Helix uses because Helix's query doesn't compile against tree-sitter-latex HEAD.

Not included:
- **SQL.** Both candidate grammars are 35–42 MB of generated C. DerekStride/tree-sitter-sql compiles to about 11 MB, more than half of the app.
- Languages without a grammar here (Java, C#, Kotlin, ...): such fences are drawn plain.

## The C runtime

`runtime/src/lib.c` is a copy of the C sources of tree-sitter 0.27.0 (MIT); only `src/wasm-stdlib/` is left out, since
WebAssembly isn't used. To upgrade, run `vendor.py --runtime <unpacked tree-sitter 0.27+ source>` and re-run
`calligrus-highlight-test`.

## How highlighting works

The steps below are implemented in `src/highlight.cpp`.

1. **Language.** Take the first word of the fence info string, up to any `,`, lowercase it, and look it up in `languages.txt`.
   Code over 256 KiB, or code that takes longer than 500 ms to parse and query, is drawn plain.
2. **Candidates.** Run `highlights.scm`. Every capture whose name has a `theme.txt` entry is a candidate.
   A name like `function.method` falls back to `function`. A colour of `-` means plain text, but the capture still claims its bytes.
3. **Overlaps.** For each byte, the winning candidate is the first when sorted by:
   length ascending, pattern index descending, capture index descending, start ascending.
   So the innermost node wins, and for the same node the pattern written last in the query wins.
   This is the same convention as tree-sitter-highlight, Neovim and Helix, so the queries are vendored in their upstream order.
   Checked against the upstream `test/highlight` assertion files, the vendored queries pass 550 of 564 assertions.
4. **Injections.** Ranges from `injections.scm` are highlighted on their own (one level deep) and painted over the host language.
5. **Spans.** Runs of the same style become spans. The goldens store theme entry names, not colours.

**Predicates.** `src/highlight.cpp` evaluates `#eq?`, `#match?` and `#any-of?` (plus their `not-`/`any-` forms) itself.
`vendor.py` drops every other predicate or directive (`#set!` in highlight queries, `#is-not? local`, …) and converts Neovim's `#lua-match?`.
It also rewrites regexes into an ASCII subset that `std::regex` (ECMAScript) handles exactly as the other engines do, and refuses anything else.

## Updating

```
python vendor.py <dir with the grammar checkouts + helix/latex.scm> --runtime <unpacked tree-sitter source>
```

Then build and run `calligrus-highlight-test`. The golden files in `tests/` are reviewed by hand: after an intended change, edit them in the same commit.
