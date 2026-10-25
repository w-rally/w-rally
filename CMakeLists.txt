cmake_minimum_required(VERSION 3.20)

# --------------------------------------------------------------------------
# WRally (docs/design.md). x64 only, self-contained: everything it builds is in
# this repository (code imported from plugin-rbr is listed in docs/imported.md).
#
#   cmake -G "Visual Studio 17 2022" -A x64 -S . -B build
#   cmake --build build --config Release
#   ctest --test-dir build -C Release
#
# Phase 1 (design 9): the game is phys-drive, linking the imported physics and
# audio as static libraries and drawing through ngg-api.dll. Phases 2-4 move
# them behind the module interfaces (include/wrally/iface).
# --------------------------------------------------------------------------

project(wrally VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WRally is x64 only: configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Everything runs from one folder laid out as the game ships (design 8).
set(WRALLY_BIN ${CMAKE_BINARY_DIR}/bin)
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} ${WRALLY_BIN})
endforeach()

include(cmake/BuildHash.cmake)

enable_testing()

# The module contract (include/wrally/iface): headers only.
add_library(wrally-iface INTERFACE)
target_include_directories(wrally-iface INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(third_party)
add_subdirectory(libs/signals)
add_subdirectory(libs/modules)
add_subdirectory(modules/physics-rbr)
add_subdirectory(modules/audio)
add_subdirectory(modules/renderer)
add_subdirectory(game)
add_subdirectory(tools/module-info)
add_subdirectory(tools/audio-check)
add_subdirectory(tests)
