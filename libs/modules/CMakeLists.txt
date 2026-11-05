# wrally-modules: ModuleHost (finding and initialising module DLLs) and FileHost (HostServices over the VFS).
add_library(wrally-modules STATIC src/module_host.cpp src/file_host.cpp)
target_include_directories(wrally-modules PUBLIC include)
target_link_libraries(wrally-modules PUBLIC wrally-iface wrally-vfs)
target_compile_definitions(wrally-modules PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(wrally-modules PRIVATE /W4 /permissive-)
