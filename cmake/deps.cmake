# Third-party libraries (included from CMakeLists.txt).
#
# By default every dependency is downloaded at configure time as a pinned release tarball (FetchContent, URL + SHA256),
# built as part of this project and linked in statically, so a plain `cmake` run builds everything and the result is
# one self-contained binary. The versions are the ones the project was developed against.
#
# -DCALLIGRUS_USE_SYSTEM_LIBS=ON instead looks for the libraries on the system (find_package / pkg-config), for
# distribution packagers. Dear ImGui is always fetched: distributions do not build it with the options this app needs
# (FreeType, 32-bit characters), and its backends are meant to be compiled into the application anyway.
#
# Targets provided either way: imgui::imgui, md4c::md4c, glfw (not Android), WebP::webp, tomlplusplus::tomlplusplus,
# harfbuzz::harfbuzz, harfbuzz::harfbuzz-subset, ZLIB::ZLIB, Freetype::Freetype, and the variable Stb_INCLUDE_DIR.
#
# Downloads are kept in CALLIGRUS_DOWNLOAD_DIR (an environment variable of the same name sets the default; CI caches
# that folder), so a fresh build folder does not download again.

option(CALLIGRUS_USE_SYSTEM_LIBS "Use the system's libraries instead of downloading and building them (ImGui is always fetched)" OFF)

include(FetchContent)

if(NOT CALLIGRUS_DOWNLOAD_DIR)
    if(DEFINED ENV{CALLIGRUS_DOWNLOAD_DIR} AND NOT "$ENV{CALLIGRUS_DOWNLOAD_DIR}" STREQUAL "")
        set(CALLIGRUS_DOWNLOAD_DIR "$ENV{CALLIGRUS_DOWNLOAD_DIR}")
    else()
        set(CALLIGRUS_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_downloads")
    endif()
endif()
# (Forward slashes: a Windows path like D:\a\... is pasted into generated CMake code, where \a is an invalid escape.)
file(TO_CMAKE_PATH "${CALLIGRUS_DOWNLOAD_DIR}" CALLIGRUS_DOWNLOAD_DIR)
file(MAKE_DIRECTORY "${CALLIGRUS_DOWNLOAD_DIR}")

# calligrus_declare(<name> <url> <sha256>): FetchContent_Declare for a pinned tarball. The archive is first fetched
# into the download folder (kept between build folders), then FetchContent unpacks that local copy. Doing the download
# here, not through FetchContent's DOWNLOAD_DIR, works the same in every CMake version (Gradle's Android CMake is 3.22).
function(calligrus_declare name url sha256)
    string(SUBSTRING "${sha256}" 0 8 short)
    set(archive "${CALLIGRUS_DOWNLOAD_DIR}/${name}-${short}.tar.gz")
    if(NOT EXISTS "${archive}")
        message(STATUS "Downloading ${url}")
        file(DOWNLOAD "${url}" "${archive}.part" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS result)
        list(GET result 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${archive}.part")
            list(GET result 1 text)
            message(FATAL_ERROR "Could not download ${url}: ${text}")
        endif()
        file(RENAME "${archive}.part" "${archive}")
    endif()
    set(extra "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        set(extra DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    FetchContent_Declare(${name} URL "${archive}" URL_HASH SHA256=${sha256} ${extra})
endfunction()

# Dear ImGui 1.92.8: no build system upstream, so the library is assembled here from the files this app uses.
calligrus_declare(imgui
    https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.tar.gz
    fecb33d33930e12ff53a34064e9d3a06c8f7c3e04408f14cd36c80e3faac863b)

# ---- the libraries that can come from the system -------------------------------------------------------------------
if(CALLIGRUS_USE_SYSTEM_LIBS)
    find_package(PkgConfig REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(Freetype REQUIRED)
    pkg_check_modules(CALLIGRUS_MD4C REQUIRED IMPORTED_TARGET md4c)
    pkg_check_modules(CALLIGRUS_WEBP REQUIRED IMPORTED_TARGET libwebp)
    pkg_check_modules(CALLIGRUS_HB REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(CALLIGRUS_HBSUBSET REQUIRED IMPORTED_TARGET harfbuzz-subset)
    add_library(md4c::md4c ALIAS PkgConfig::CALLIGRUS_MD4C)
    add_library(WebP::webp ALIAS PkgConfig::CALLIGRUS_WEBP)
    add_library(harfbuzz::harfbuzz ALIAS PkgConfig::CALLIGRUS_HB)
    add_library(harfbuzz::harfbuzz-subset ALIAS PkgConfig::CALLIGRUS_HBSUBSET)
    # toml++ ships a CMake package (tomlplusplus::tomlplusplus) and a pkg-config file; accept either.
    find_package(tomlplusplus CONFIG QUIET)
    if(NOT TARGET tomlplusplus::tomlplusplus)
        pkg_check_modules(CALLIGRUS_TOML REQUIRED IMPORTED_TARGET tomlplusplus)
        add_library(tomlplusplus::tomlplusplus ALIAS PkgConfig::CALLIGRUS_TOML)
    endif()
    if(NOT ANDROID)
        find_package(glfw3 CONFIG QUIET)
        if(NOT TARGET glfw)
            pkg_check_modules(CALLIGRUS_GLFW REQUIRED IMPORTED_TARGET glfw3)
            add_library(glfw ALIAS PkgConfig::CALLIGRUS_GLFW)
        endif()
    endif()
    find_path(Stb_INCLUDE_DIR stb_image.h PATH_SUFFIXES stb REQUIRED)
else()
    # Pinned release tarballs.
    calligrus_declare(zlib
        https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
        b99a0b86c0ba9360ec7e78c4f1e43b1cbdf1e6936c8fa0f6835c0cd694a495a1)
    calligrus_declare(freetype
        https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz
        dc49de6b01a266eef4876a4dd34d9842c475d3e28ff2eff63bd2fb760ab56261)
    calligrus_declare(harfbuzz
        https://github.com/harfbuzz/harfbuzz/archive/refs/tags/14.4.0.tar.gz
        46dc4f3b6aefc4d8256b10017186f5ebe50ea086714ab8948cdac4695a7a80a8)
    calligrus_declare(md4c
        https://github.com/mity/md4c/archive/refs/tags/release-0.5.3.tar.gz
        353c346f376b87c954a13f3415ede2d51264cc61dc5abcd38ff1d2aa0d059b9e)
    calligrus_declare(libwebp
        https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
        93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136)
    calligrus_declare(tomlplusplus
        https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
        8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155)
    calligrus_declare(stb
        https://github.com/nothings/stb/archive/f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31.tar.gz
        bc6ccf08bec08fea8ef423c7117dca06d2f62d2b27c5485f6865584b533fa7fa)
    if(NOT ANDROID)
        calligrus_declare(glfw
            https://github.com/glfw/glfw/archive/refs/tags/3.5.1.tar.gz
            5234f4f29473e9a06bc7847d8371858dd135d38466eeeaa652fdc9f8f9ff0c20)
    endif()

    # The libraries' own CMake files are written for many setups: build static, no tools, no tests, no install rules,
    # and let policies they predate take the modern behaviour (CMP0091 is what makes /MT apply to them).
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # (the Android app is a shared library)

    # zlib: only the static library (the target is zlibstatic).
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()

    # FreeType: the bundled fonts are plain TrueType/OpenType outlines, so none of its optional helpers (PNG bitmaps,
    # Brotli-compressed WOFF2, bzip2, HarfBuzz) are needed. (Its gzip reader uses a private copy of zlib.)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(freetype)
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()

    # HarfBuzz: shaping and subsetting only (no FreeType glue, no GPU/vector/raster extras, no utilities).
    set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET ON CACHE BOOL "" FORCE)
    set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_GPU OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(harfbuzz)
    # (Static libraries: list harfbuzz before harfbuzz-subset when linking. The subset library repeats a few of the main
    # library's sources, which only clash if the linker takes them from the subset library first.)
    add_library(harfbuzz::harfbuzz ALIAS harfbuzz)
    add_library(harfbuzz::harfbuzz-subset ALIAS harfbuzz-subset)

    # md4c (the parser only, not md2html).
    set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(md4c)
    add_library(md4c::md4c ALIAS md4c)

    # libwebp: the decoder is all the app uses; no tools.
    foreach(opt ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO WEBPMUX EXTRAS LIBWEBPMUX)
        set(WEBP_BUILD_${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(WEBP_LINK_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libwebp)
    add_library(WebP::webp ALIAS webp)
    # The sources include <webp/decode.h>.
    target_include_directories(webp INTERFACE "$<BUILD_INTERFACE:${libwebp_SOURCE_DIR}/src>")

    # toml++ (header-only; provides tomlplusplus::tomlplusplus itself).
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tomlplusplus)

    # stb: header-only, no build system; Stb_INCLUDE_DIR is the folder with stb_image.h.
    FetchContent_MakeAvailable(stb)
    set(Stb_INCLUDE_DIR "${stb_SOURCE_DIR}")

    # GLFW (desktop windowing). On Windows it links the C runtime statically like everything else.
    if(NOT ANDROID)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(glfw)
    endif()
endif()

# ---- Dear ImGui (always fetched) ----------------------------------------------------------------------------------
FetchContent_MakeAvailable(imgui)
set(CALLIGRUS_IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
if(NOT ANDROID)
    list(APPEND CALLIGRUS_IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
endif()
add_library(imgui STATIC ${CALLIGRUS_IMGUI_SOURCES})
add_library(imgui::imgui ALIAS imgui)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/freetype)
# 32-bit characters (emoji above U+FFFF) and FreeType glyph rendering.
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32 IMGUI_ENABLE_FREETYPE)
target_link_libraries(imgui PUBLIC Freetype::Freetype ${CMAKE_DL_LIBS})
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(ANDROID)
    # The Android window layer is the app's own (src/android); the renderer is OpenGL ES 3.
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
    target_link_libraries(imgui PUBLIC GLESv3)
else()
    target_link_libraries(imgui PUBLIC glfw)
endif()
if(MSVC)
    target_compile_options(imgui PRIVATE /utf-8)
endif()
