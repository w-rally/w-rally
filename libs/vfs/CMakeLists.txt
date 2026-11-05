# wrally-vfs: where W-Rally finds a file - the downloaded layer, the W-Rally layer and the RBR folder
# (docs/content-layers.md). phys-data for physics.rbz, which only the VFS's RBR mapping opens.
add_library(wrally-vfs STATIC src/vfs.cpp)
target_include_directories(wrally-vfs PUBLIC include)
target_link_libraries(wrally-vfs PUBLIC wrally-json PRIVATE phys-data)
target_compile_definitions(wrally-vfs PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(wrally-vfs PRIVATE /W4 /permissive-)
