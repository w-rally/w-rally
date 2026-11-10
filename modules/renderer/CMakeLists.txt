# The NGG renderer (imported from plugin-rbr ngg/, docs/imported.md): ngg-core (D3D12), behind RendererApi as
# WRenderer.dll (src/module: the module, and the UI pass).
#
# The shaders are HLSL files in shaders/ (docs/ngg-design.md, "Shaders and the pipeline cache"), compiled at BUILD
# time: ngg-shaderc (src/shaderc) compiles every line of shaders/shaders.txt with FXC or the vendored DXC
# (third_party/dxc, which signs with its dxil.dll) into generated/ngg_shaders.cpp, linked into ngg-core. The game
# compiles nothing; NGG_SHADER_DIR=<this folder>/shaders compiles from the files at run time instead (core/shaders.h),
# for which dxcompiler.dll / dxil.dll are still copied beside the module in the build tree (not installed).
add_library(ngg-core STATIC src/core/device.cpp src/core/aerial.cpp src/core/renderer.cpp src/core/atmosphere_ms.cpp
            src/core/shader_compiler.cpp src/core/shaders.cpp src/core/env.cpp src/core/log.cpp src/core/mipgen.cpp
            src/core/post.cpp src/core/rt.cpp
            src/core/scene.cpp src/core/shadow.cpp src/core/sky.cpp src/core/weather.cpp
            src/core/astronomy.cpp src/core/lights.cpp src/core/fsr.cpp src/core/dlss.cpp src/core/gtao.cpp
            src/core/visibility.cpp src/core/probe.cpp src/core/models.cpp)
target_include_directories(ngg-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_definitions(ngg-core PRIVATE NGG_SHADER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
# Intel XeGTAO (third_party/xegtao, MIT, unchanged): shaders/gtao.hlsl includes its HLSL; XeGTAO.h is also the C++
# side's constants.
set(XEGTAO_DIR ${PROJECT_SOURCE_DIR}/third_party/xegtao)
target_include_directories(ngg-core PRIVATE ${XEGTAO_DIR})

# The build-time shader compiler and its output. Any shader file, the list or the tool changed: all compiled again
# (in parallel, a few seconds).
add_executable(ngg-shaderc src/shaderc/main.cpp src/core/shader_compiler.cpp)
target_include_directories(ngg-shaderc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(ngg-shaderc PRIVATE d3dcompiler)
target_compile_definitions(ngg-shaderc PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(ngg-shaderc PRIVATE /W4)
set_target_properties(ngg-shaderc PROPERTIES CXX_STANDARD 17)
file(GLOB NGG_SHADER_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsl
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsli)
set(NGG_SHADERS_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/ngg_shaders.cpp)
add_custom_command(OUTPUT ${NGG_SHADERS_CPP}
                   COMMAND ngg-shaderc --manifest ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.txt
                           --dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders --dxc ${PROJECT_SOURCE_DIR}/third_party/dxc
                           --out ${NGG_SHADERS_CPP}
                   DEPENDS ngg-shaderc ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.txt ${NGG_SHADER_FILES}
                           ${XEGTAO_DIR}/XeGTAO.h ${XEGTAO_DIR}/XeGTAO.hlsli
                   COMMENT "ngg-shaderc: compiling the renderer's shaders"
                   VERBATIM)
target_sources(ngg-core PRIVATE ${NGG_SHADERS_CPP})
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
target_link_libraries(WRenderer PRIVATE ngg-core wrally-iface wrally-png)
target_compile_definitions(WRenderer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(WRenderer PRIVATE /W4)
set_target_properties(WRenderer PROPERTIES CXX_STANDARD 17)
wrally_module_output(WRenderer renderer)
# The build hash over the shaders too (they were C++ literals in src/ once).
set(shader_hashes "")
foreach(f ${NGG_SHADER_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.txt)
    file(SHA256 ${f} h)
    string(APPEND shader_hashes " ${h}")
endforeach()
wrally_build_hash(WRenderer WRENDERER_BUILD_HASH ${CMAKE_CURRENT_SOURCE_DIR}/src EXTRA "${shader_hashes}")

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
