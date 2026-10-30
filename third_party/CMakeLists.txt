# Dear ImGui 1.91.5 (imgui/, MIT, unchanged from github.com/ocornut/imgui) with its Win32 platform backend; the
# rendering backend is the renderer's UI pass (wrally/iface/renderer.h UiDrawList). Configured by
# imgui_config/wrally_imconfig.h.
add_library(imgui STATIC imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp
            imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC imgui imgui/backends imgui_config)
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="wrally_imconfig.h")
set_target_properties(imgui PROPERTIES CXX_STANDARD 17)

# stb_vorbis v1.22 (stb/stb_vorbis.c, public domain, unchanged from github.com/nothings/stb): the Ogg Vorbis decoder the
# audio module reads the co-driver's speech with (audio/sample.h ReadOgg). C, so the C compiler for this one file.
enable_language(C)
add_library(stb-vorbis STATIC stb/stb_vorbis.c)
target_include_directories(stb-vorbis PUBLIC stb)
target_compile_definitions(stb-vorbis PRIVATE _CRT_SECURE_NO_WARNINGS STB_VORBIS_NO_PUSHDATA_API)
target_compile_options(stb-vorbis PRIVATE /W0)
