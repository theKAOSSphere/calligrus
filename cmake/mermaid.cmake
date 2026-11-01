# Pure C++ Mermaid renderer (work in progress; see src/mermaid/PORTING.md).
#
# A port of the Rust crate mermaid-rs-renderer 0.3.1; the app draws every diagram with it
# (src/math.cpp). The sources live in src/mermaid/. The test target compares our output with
# golden SVGs made by the Rust crate (tests/mermaid/*.svg, from scripts/mermaid-oracle).
set(MERMAID_SOURCES
    ${CMAKE_SOURCE_DIR}/src/mermaid/parser.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/parser_graphs.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/fontmetrics.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/json5.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/math_label.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kinds.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_pie.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_c4.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_gitgraph.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_mindmap.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_architecture.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_block.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_zenuml.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_sankey.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_requirement.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_treemap.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_packet.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_util.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_kanban.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_radar.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_xychart.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_quadrant.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_gantt.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_journey.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/kind_timeline.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/text.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/ranking.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/layout.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/routing.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/router.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/path_cleanup.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/edge_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/label_placement.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/label_placement_flowchart.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/subgraphs.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/sequence.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/render.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/svg_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/textface.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/svg_raster.cpp
    ${CMAKE_SOURCE_DIR}/src/mermaid/native.cpp
)
# The tiny-skia port in src/texmath/canvas.cpp (which also fills the shapes here) is already part
# of the TeX sources; the test target adds it by itself below.

# The app.
target_sources(calligrus PRIVATE ${MERMAID_SOURCES})
if(MSVC)
    # /fp:strict: no fused multiply-add or other float contraction, so f32 results match Rust's.
    set_source_files_properties(${MERMAID_SOURCES} PROPERTIES COMPILE_OPTIONS /fp:strict)
else()
    set_source_files_properties(${MERMAID_SOURCES} PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

if(NOT ANDROID)  # (the tests are for desktop builds)
add_executable(calligrus-mermaid-test src/mermaid/mermaid_test.cpp ${MERMAID_SOURCES} ${CMAKE_SOURCE_DIR}/src/texmath/canvas.cpp)
target_include_directories(calligrus-mermaid-test PRIVATE ${Stb_INCLUDE_DIR})  # stb_image: PNG icons in <image>
target_link_libraries(calligrus-mermaid-test PRIVATE harfbuzz::harfbuzz)  # text shaping + glyph outlines
target_compile_definitions(calligrus-mermaid-test PRIVATE _CRT_SECURE_NO_WARNINGS
    CALLIGRUS_MERMAID_CORPUS="${CMAKE_SOURCE_DIR}/tests/mermaid"
    CALLIGRUS_MERMAID_FONT="${CALLIGRUS_DATA_DIR}/assets/fonts/IBMPlexSans-Regular.ttf")
if(MSVC)
    # /fp:strict: no fused multiply-add or other float contraction, so f32 results match Rust's.
    target_compile_options(calligrus-mermaid-test PRIVATE /utf-8 /W4 /permissive-  /fp:strict)
else()
    target_compile_options(calligrus-mermaid-test PRIVATE -Wall -Wextra -ffp-contract=off)
endif()
endif()
