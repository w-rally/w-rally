# The NGG renderer (imported from plugin-rbr ngg/, docs/imported.md): ngg-core, D3D12, and ngg-api.dll, the flat C
# surface the game draws through in phase 1. Phase 4 puts ngg-core behind RendererApi as WRenderer.dll.
#
# Shader model 6 shaders compile at run time through dxcompiler.dll / dxil.dll (core/dxc.cpp loads them by name), so
# both are copied beside the DLLs that need them.
add_library(ngg-core STATIC src/core/device.cpp src/core/aerial.cpp src/core/renderer.cpp src/core/atmosphere_ms.cpp
            src/core/dxc.cpp src/core/env.cpp src/core/log.cpp src/core/mipgen.cpp src/core/post.cpp src/core/rt.cpp
            src/core/scene.cpp src/core/shadow.cpp src/core/sky.cpp src/core/weather.cpp)
target_include_directories(ngg-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(ngg-core PUBLIC d3d12 dxgi d3dcompiler)
target_compile_definitions(ngg-core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ngg-core PRIVATE /W3)

# NGG_API_EXPORTS is defined by ngg_api.cpp itself, right before its own header.
add_library(ngg-api SHARED src/api/ngg_api.cpp)
target_include_directories(ngg-api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/api)
target_link_libraries(ngg-api PRIVATE ngg-core)
target_compile_definitions(ngg-api PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ngg-api PRIVATE /W3)

foreach(t ngg-core ngg-api)
    set_target_properties(${t} PROPERTIES CXX_STANDARD 17)
endforeach()

set(WRALLY_DXC_DLLS ${PROJECT_SOURCE_DIR}/third_party/dxc/dxcompiler.dll ${PROJECT_SOURCE_DIR}/third_party/dxc/dxil.dll)
add_custom_command(TARGET ngg-api POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WRALLY_DXC_DLLS}
                   $<TARGET_FILE_DIR:ngg-api>)
