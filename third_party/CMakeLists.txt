# Dear ImGui 1.91.5 (imgui/, MIT, unchanged from github.com/ocornut/imgui) with its Win32 platform backend; the
# rendering backend is the renderer's UI pass (wrally/iface/renderer.h UiDrawList). Configured by
# imgui_config/wrally_imconfig.h.
add_library(imgui STATIC imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp
            imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC imgui imgui/backends imgui_config)
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="wrally_imconfig.h")
set_target_properties(imgui PROPERTIES CXX_STANDARD 17)
