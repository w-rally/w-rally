# The NGG renderer (imported from plugin-rbr ngg/, docs/imported.md): ngg-core (D3D12), behind RendererApi as
# WRenderer.dll (src/module: the module, and the UI pass).
#
# Shader model 6 shaders compile at run time through dxcompiler.dll / dxil.dll (core/dxc.cpp loads them by name), so
# both are copied beside the module that needs them.
add_library(ngg-core STATIC src/core/device.cpp src/core/aerial.cpp src/core/renderer.cpp src/core/atmosphere_ms.cpp
            src/core/dxc.cpp src/core/env.cpp src/core/log.cpp src/core/mipgen.cpp src/core/post.cpp src/core/rt.cpp
            src/core/scene.cpp src/core/shadow.cpp src/core/sky.cpp src/core/weather.cpp
            src/core/astronomy.cpp src/core/lights.cpp src/core/fsr.cpp src/core/dlss.cpp)
target_include_directories(ngg-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(ngg-core PUBLIC d3d12 dxgi d3dcompiler PRIVATE fidelityfx)
# DLSS (core/dlss.cpp) when its SDK was fetched (third_party/CMakeLists.txt). NVIDIA's NGX library is built against the
# static release C runtime (/MT), so a Debug build leaves DLSS out (dlss.cpp's stub: no DLSS, logged).
if(TARGET nvidia-dlss)
    target_link_libraries(ngg-core PRIVATE $<$<NOT:$<CONFIG:Debug>>:nvidia-dlss>)
    target_include_directories(ngg-core PRIVATE $<$<NOT:$<CONFIG:Debug>>:${WRALLY_DLSS_DIR}/include>)
    target_compile_definitions(ngg-core PRIVATE $<$<NOT:$<CONFIG:Debug>>:WRALLY_DLSS>)
endif()
target_compile_definitions(ngg-core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ngg-core PRIVATE /W3)
set_target_properties(ngg-core PROPERTIES CXX_STANDARD 17)

# ngg-format: the .ngg in memory, its writer and a strict reader, beside the format header the renderer's loader
# reads by (docs/design.md decision 2) - for the game's legacy importer and the tools.
add_library(ngg-format STATIC src/format/ngg_scene.cpp)
target_include_directories(ngg-format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/format)
target_compile_options(ngg-format PRIVATE /W4)
set_target_properties(ngg-format PROPERTIES CXX_STANDARD 17)

add_library(WRenderer SHARED src/module/module.cpp src/module/ui.cpp)
target_include_directories(WRenderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(WRenderer PRIVATE ngg-core wrally-iface)
target_compile_definitions(WRenderer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(WRenderer PRIVATE /W4)
set_target_properties(WRenderer PROPERTIES CXX_STANDARD 17)
wrally_module_output(WRenderer renderer)
wrally_build_hash(WRenderer WRENDERER_BUILD_HASH ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(WRALLY_DXC_DLLS ${PROJECT_SOURCE_DIR}/third_party/dxc/dxcompiler.dll ${PROJECT_SOURCE_DIR}/third_party/dxc/dxil.dll)
add_custom_command(TARGET WRenderer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WRALLY_DXC_DLLS}
                   $<TARGET_FILE_DIR:WRenderer>)
# FSR (core/fsr.cpp): AMD's signed ffx-api DLLs, fetched by third_party/CMakeLists.txt, beside the module that loads them.
if(WRALLY_FFX_DLLS)
    add_custom_command(TARGET WRenderer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WRALLY_FFX_DLLS}
                       $<TARGET_FILE_DIR:WRenderer>)
endif()
# DLSS (core/dlss.cpp): NVIDIA's runtime, fetched by third_party/CMakeLists.txt, beside the module NGX finds it from.
if(WRALLY_DLSS_DLLS)
    add_custom_command(TARGET WRenderer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WRALLY_DLSS_DLLS}
                       $<TARGET_FILE_DIR:WRenderer>)
endif()
