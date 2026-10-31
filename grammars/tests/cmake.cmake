cmake_minimum_required(VERSION 3.21)
project(demo LANGUAGES C CXX)

# Options
option(DEMO_TESTS "Build tests" ON)
set(SOURCES src/main.cpp src/util.cpp)

add_executable(demo ${SOURCES})
target_compile_definitions(demo PRIVATE DEMO_VERSION="${PROJECT_VERSION}")
if(MSVC AND NOT DEMO_TESTS)
    target_compile_options(demo PRIVATE /W4)
endif()
list(APPEND SOURCES extra.cpp)
foreach(f IN LISTS SOURCES)
    message(STATUS "source: ${f} $ENV{HOME}")
endforeach()
