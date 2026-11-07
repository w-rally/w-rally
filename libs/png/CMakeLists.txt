# wrally-png: a picture to a .png file, through Windows' own codecs (WIC) - the screenshots and the renderer's captures.
add_library(wrally-png STATIC src/png.cpp)
target_include_directories(wrally-png PUBLIC include)
target_link_libraries(wrally-png PRIVATE windowscodecs ole32)
target_compile_definitions(wrally-png PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(wrally-png PRIVATE /W4 /permissive-)
set_target_properties(wrally-png PROPERTIES CXX_STANDARD 17)
