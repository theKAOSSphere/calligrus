# TeX math engine (included from CMakeLists.txt).
#
# src/texmath/ is a C++ TeX math engine: parser, symbol tables, TeX layout, rasterizer, plus a small
# OpenType reader (font.cpp, cff.cpp) and a port of the tiny-skia rasterizer paths it uses (canvas.cpp).
set(TEXMATH_SOURCES
    ${CMAKE_SOURCE_DIR}/src/texmath/texmath.cpp
    ${CMAKE_SOURCE_DIR}/src/texmath/parse.cpp
    ${CMAKE_SOURCE_DIR}/src/texmath/symbols.cpp
    ${CMAKE_SOURCE_DIR}/src/texmath/layout.cpp
    ${CMAKE_SOURCE_DIR}/src/texmath/raster.cpp
    ${CMAKE_SOURCE_DIR}/src/texmath/font.cpp
    ${CMAKE_SOURCE_DIR}/src/texmath/cff.cpp
    ${CMAKE_SOURCE_DIR}/src/texmath/canvas.cpp
)
target_sources(calligrus PRIVATE ${TEXMATH_SOURCES})

# Corpus, fuzz and abuse checks for the C++ engine.
# Console only, no window.
add_executable(calligrus-texmath-test src/texmath/texmath_test.cpp ${TEXMATH_SOURCES})
if(MSVC)
    target_compile_options(calligrus-texmath-test PRIVATE /utf-8 /W4 /permissive-)
endif()
# The test loads the math font from <exe dir>/assets/fonts, like the app.
add_custom_command(TARGET calligrus-texmath-test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:calligrus-texmath-test>/assets/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/assets/fonts/latinmodern-math.otf
            $<TARGET_FILE_DIR:calligrus-texmath-test>/assets/fonts/latinmodern-math.otf)
