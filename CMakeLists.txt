# calligrus: a small native Markdown viewer (Dear ImGui + GLFW + OpenGL 3, md4c parser).
#
# Dependencies are downloaded and built by CMake itself (FetchContent, see cmake/deps.cmake): a plain
#   cmake -S . -B build && cmake --build build --config Release
# is all it takes. Pass -DCALLIGRUS_USE_SYSTEM_LIBS=ON to use the system's libraries instead.
cmake_minimum_required(VERSION 3.21)

# Single-file exe on Windows: every library is linked in statically, and so is the C/C++ runtime (/MT instead of
# /MD), so calligrus.exe needs no DLLs next to it. Must be set before project(). (Ignored by other compilers.)
if(WIN32 AND NOT CALLIGRUS_USE_SYSTEM_LIBS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(calligrus VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Per-OS pieces: platform.cpp (Win32), platform_android.cpp (Android: the app is a shared library
# loaded by GameActivity, see cmake/android.cmake) or platform_linux.cpp, and the system libraries
# the small console tools need besides their own code.
if(WIN32)
    set(CALLIGRUS_PLATFORM_SRC src/platform.cpp)
    set(CALLIGRUS_PLATFORM_LIBS shell32 comdlg32 dwmapi advapi32)
elseif(ANDROID)
    set(CALLIGRUS_PLATFORM_SRC src/android/platform_android.cpp)
    set(CALLIGRUS_PLATFORM_LIBS "")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unknown-pragmas>)  # MSVC-only `#pragma warning` lines
else()
    set(CALLIGRUS_PLATFORM_SRC src/platform_linux.cpp)
    set(CALLIGRUS_PLATFORM_LIBS "")
    find_package(Threads REQUIRED)
    link_libraries(Threads::Threads)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unknown-pragmas>)  # MSVC-only `#pragma warning` lines
endif()

# Third-party libraries (FetchContent, or the system's with CALLIGRUS_USE_SYSTEM_LIBS): imgui, md4c, glfw, stb,
# libwebp, toml++, HarfBuzz, zlib, FreeType.
include(cmake/deps.cmake)
if(NOT ANDROID)  # (Android: EGL and OpenGL ES come from the NDK, the window from GameActivity)
    find_package(OpenGL REQUIRED)
endif()

# Shared data lives in this repository (themes, grammars, the spec's test files).
set(CALLIGRUS_DATA_DIR ${CMAKE_SOURCE_DIR})

# Embedded fonts, themes and demo document (library `calligrus-resources`).
include(cmake/resources.cmake)

set(CALLIGRUS_SOURCES
    src/main.cpp
    src/model.cpp
    src/org.cpp
    src/slides.cpp
    src/imagefilter.cpp
    src/marks.cpp
    src/pages.cpp
    src/sysfont.cpp
    src/fonts.cpp
    src/shaping.cpp
    src/prose_shaping.cpp
    src/shaped_draw.cpp
    src/font_index.cpp
    src/inline_layout.cpp
    src/text_capture.cpp
    src/render.cpp
    ${CALLIGRUS_PLATFORM_SRC}
    src/math.cpp
    src/images.cpp
    src/select.cpp
    src/find.cpp
    src/navigation.cpp
    src/config.cpp
    src/userdir.cpp
    src/theme.cpp
    src/syntax_entries.cpp
    src/settings.cpp
    src/debug_capture.cpp
    src/session.cpp
    src/script.cpp
    src/editor.cpp
    src/pdf.cpp
    src/touch.cpp
)
if(ANDROID)
    # The app is a shared library that GameActivity loads (the Java side: android/).
    add_library(calligrus SHARED ${CALLIGRUS_SOURCES})
else()
    add_executable(calligrus ${CALLIGRUS_SOURCES})
endif()
if(WIN32)
    target_sources(calligrus PRIVATE src/app.rc)  # exe icon + version info
endif()
target_include_directories(calligrus PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(calligrus PRIVATE WebP::webp tomlplusplus::tomlplusplus harfbuzz::harfbuzz harfbuzz::harfbuzz-subset ZLIB::ZLIB Freetype::Freetype calligrus-resources)

target_link_libraries(calligrus PRIVATE
    imgui::imgui
    md4c::md4c
)
if(NOT ANDROID)
    target_link_libraries(calligrus PRIVATE glfw OpenGL::GL)
endif()
if(WIN32)
    target_link_libraries(calligrus PRIVATE
        comdlg32   # GetOpenFileNameW (native "Open" dialog)
        shell32    # ShellExecuteW (open links in the browser), CommandLineToArgvW
        dwmapi     # dark title bar
    )
endif()

# The version shown by `calligrus --version` (keep the release tag in step).
target_compile_definitions(calligrus PRIVATE CALLIGRUS_VERSION="${PROJECT_VERSION}")

if(MSVC)
    # UTF-8 source/execution charset, warnings on.
    target_compile_options(calligrus PRIVATE /utf-8 /W4 /permissive-)
    # A GUI app: no console window, but keep the normal `int main()` entry point.
    set_target_properties(calligrus PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(calligrus PRIVATE /ENTRY:mainCRTStartup)
endif()

# The console tools and tests are for desktop builds only.
if(NOT ANDROID)

# Console tool that prints the parsed Block/Span tree (parser debugging, no window).
add_executable(calligrus-dump src/dump.cpp src/model.cpp src/org.cpp ${CALLIGRUS_PLATFORM_SRC})
target_link_libraries(calligrus-dump PRIVATE md4c::md4c ${CALLIGRUS_PLATFORM_LIBS})
if(MSVC)
    target_compile_options(calligrus-dump PRIVATE /utf-8 /W4 /permissive-)
endif()

# Front matter / footnote / callout preprocessing tests (console, no window).
add_executable(calligrus-model-test src/model_test.cpp src/model.cpp src/org.cpp src/slides.cpp src/imagefilter.cpp src/marks.cpp)
target_link_libraries(calligrus-model-test PRIVATE md4c::md4c)
if(MSVC)
    target_compile_options(calligrus-model-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Selection logic tests (console, no window):
add_executable(calligrus-select-test src/select_test.cpp src/select.cpp)
target_link_libraries(calligrus-select-test PRIVATE imgui::imgui)
if(MSVC)
    target_compile_options(calligrus-select-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Headless script-mode parser tests (console, no window).
add_executable(calligrus-script-test src/script_test.cpp src/script.cpp src/editor.cpp src/pdf.cpp src/pages.cpp)
target_include_directories(calligrus-script-test PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(calligrus-script-test PRIVATE harfbuzz::harfbuzz harfbuzz::harfbuzz-subset ZLIB::ZLIB)
if(MSVC)
    target_compile_options(calligrus-script-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Editor protocol parser tests (console, no window).
add_executable(calligrus-editor-test src/editor_test.cpp src/editor.cpp)
if(MSVC)
    target_compile_options(calligrus-editor-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Input logic tests (console, no window): zoom steps, vim keys, heading jumps,
# middle-click autoscroll, find matching.
add_executable(calligrus-nav-test src/nav_test.cpp src/navigation.cpp src/find.cpp src/select.cpp)
target_link_libraries(calligrus-nav-test PRIVATE imgui::imgui)
if(MSVC)
    target_compile_options(calligrus-nav-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Touch gestures (drag, tap, fling, pinch): synthetic finger events with timestamps (console, no window).
add_executable(calligrus-touch-test src/touch_test.cpp src/touch.cpp)
if(MSVC)
    target_compile_options(calligrus-touch-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Link / history / state-file tests (console, no window).
add_executable(calligrus-session-test src/session_test.cpp src/session.cpp src/userdir.cpp ${CALLIGRUS_PLATFORM_SRC})
target_link_libraries(calligrus-session-test PRIVATE tomlplusplus::tomlplusplus ${CALLIGRUS_PLATFORM_LIBS})
if(MSVC)
    target_compile_options(calligrus-session-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Config / writer / theme / syntax-lookup tests: runs every file of the shared
# test suite in spec/tests (console, no window).
add_executable(calligrus-config-test src/config_test.cpp src/config.cpp src/userdir.cpp src/theme.cpp src/syntax_entries.cpp)
target_link_libraries(calligrus-config-test PRIVATE imgui::imgui tomlplusplus::tomlplusplus calligrus-resources)
target_compile_definitions(calligrus-config-test PRIVATE
    CALLIGRUS_SPEC_TESTS_DIR="${CALLIGRUS_DATA_DIR}/spec/tests" CALLIGRUS_GRAMMARS_DIR="${CALLIGRUS_DATA_DIR}/grammars")
if(MSVC)
    target_compile_options(calligrus-config-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Fonts by family name (spec 8a): the shared fontpick-*.txt fixtures against the
# resolver, the resolve_fonts rules, and the real system-font scanner (console, no window).
add_executable(calligrus-fonts-test src/fonts_test.cpp src/font_index.cpp src/fonts.cpp src/shaping.cpp src/prose_shaping.cpp ${CALLIGRUS_PLATFORM_SRC})
target_link_libraries(calligrus-fonts-test PRIVATE harfbuzz::harfbuzz imgui::imgui calligrus-resources ${CALLIGRUS_PLATFORM_LIBS})
target_compile_definitions(calligrus-fonts-test PRIVATE CALLIGRUS_SPEC_TESTS_DIR="${CALLIGRUS_DATA_DIR}/spec/tests")
if(MSVC)
    target_compile_options(calligrus-fonts-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Text-sharpness tool: draws a line of text like the app, to a PNG (console, no
# window). Uses the embedded fonts.
add_executable(calligrus-glyphdump src/glyphdump.cpp src/fonts.cpp src/shaping.cpp src/prose_shaping.cpp src/prose_shaping.cpp ${CALLIGRUS_PLATFORM_SRC})
target_include_directories(calligrus-glyphdump PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(calligrus-glyphdump PRIVATE harfbuzz::harfbuzz imgui::imgui calligrus-resources ${CALLIGRUS_PLATFORM_LIBS})
if(MSVC)
    target_compile_options(calligrus-glyphdump PRIVATE /utf-8 /W4 /permissive-)
endif()

endif()  # NOT ANDROID

if(NOT MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unknown-pragmas>)  # MSVC-only `#pragma warning` lines
endif()

# Subsystems with their own build rules (sources added to `calligrus`, test tools).
include(cmake/highlight.cmake)  # syntax highlighting
include(cmake/texmath.cmake)    # TeX math engine
include(cmake/mermaid.cmake)    # pure C++ Mermaid renderer (work in progress, opt-in)

if(ANDROID)
    include(cmake/android.cmake)  # the Android shell: window, EGL, touch, JNI
endif()

# No post-build copying: fonts, themes and the demo are inside the exe
# (cmake/resources.cmake), so build\Release\calligrus.exe runs on its own.

if(NOT ANDROID)

# Page-break rules tests (console, no window):
add_executable(calligrus-pages-test src/pages_test.cpp src/pages.cpp)
if(MSVC)
    target_compile_options(calligrus-pages-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# System fonts for missing characters (spec 30): the choice logic on made-up faces, the cmap check on
# the bundled fonts, the document's characters (console, no window). Same cases as sysfont.rs.
add_executable(calligrus-sysfont-test src/sysfont_test.cpp src/sysfont.cpp src/font_index.cpp src/model.cpp src/org.cpp src/slides.cpp src/imagefilter.cpp src/marks.cpp ${CALLIGRUS_PLATFORM_SRC})
target_link_libraries(calligrus-sysfont-test PRIVATE md4c::md4c imgui::imgui calligrus-resources ${CALLIGRUS_PLATFORM_LIBS})
if(MSVC)
    target_compile_options(calligrus-sysfont-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Complex scripts in prose (spec 32): script runs, font choice, clusters, and real HarfBuzz shaping
# of Devanagari with the test font in spec/tests/fonts (console, no window).
add_executable(calligrus-prose-shaping-test src/prose_shaping_test.cpp src/prose_shaping.cpp)
target_link_libraries(calligrus-prose-shaping-test PRIVATE harfbuzz::harfbuzz)
target_compile_definitions(calligrus-prose-shaping-test PRIVATE CALLIGRUS_SPEC_TESTS_DIR="${CALLIGRUS_DATA_DIR}/spec/tests")
if(MSVC)
    target_compile_options(calligrus-prose-shaping-test PRIVATE /utf-8 /W4 /permissive-)
endif()

# PDF writer (spec 33): CID fonts, subsetting, ToUnicode, predictor images; the bundled Plex font (console, no window).
add_executable(calligrus-pdf-test src/pdf_test.cpp src/pdf.cpp)
target_include_directories(calligrus-pdf-test PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(calligrus-pdf-test PRIVATE harfbuzz::harfbuzz harfbuzz::harfbuzz-subset ZLIB::ZLIB calligrus-resources)
if(MSVC)
    target_compile_options(calligrus-pdf-test PRIVATE /utf-8 /W4 /permissive-)
endif()

endif()  # NOT ANDROID
