# helper for Stage 3b: python addkind2.py Name lower [norender|parseonly]
# registers kind in kinds.h/kinds.cpp/parser.cpp/cmake (re-reads each file right before editing)
#   norender : own parser + layout, drawn by the generic graph writer
#   parseonly: own parser only (layout = flowchart layout, writer = generic graph writer)
import sys
Name, lower = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else ''
def rep(p, a, b):
    s = open(p, encoding='utf-8').read()
    assert a in s, (p, a)
    open(p, 'w', encoding='utf-8').write(s.replace(a, b, 1))
rep('kinds.h', "// per-kind layout / writer entry points", f"Graph parse_{lower}_diagram(const std::string& input);\n\n// per-kind layout / writer entry points")
if mode != 'parseonly':
    decl = f"Layout compute_{lower}_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);\n"
    if mode != 'norender':
        decl += f"std::string render_{lower}_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);\n"
    rep('kinds.h', "}  // namespace mermaid", decl + "\n}  // namespace mermaid")
    rep('kinds.cpp', "    default: return std::nullopt;\n    }\n}\n\nstd::optional<std::string> render_other_svg", f"    case DiagramKind::{Name}: return compute_{lower}_layout(graph, theme, config);\n    default: return std::nullopt;\n    }}\n}}\n\nstd::optional<std::string> render_other_svg")
    if mode != 'norender':
        rep('kinds.cpp', "    default: return std::nullopt;\n    }\n}\n\n}  // namespace mermaid", f"    case DiagramKind::{Name}: return render_{lower}_svg(layout, theme, config);\n    default: return std::nullopt;\n    }}\n}}\n\n}}  // namespace mermaid")
rep('parser.cpp', "    default: throw Unsupported(\"diagram type not ported yet\");", f"    case DiagramKind::{Name}: return parse_{lower}_diagram(input);\n    default: throw Unsupported(\"diagram type not ported yet\");")
rep('../../cmake/mermaid.cmake', "    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_pie.cpp\n", f"    ${{CMAKE_SOURCE_DIR}}/src/mermaid/kind_pie.cpp\n    ${{CMAKE_SOURCE_DIR}}/src/mermaid/kind_{lower}.cpp\n")
print("registered", Name)
