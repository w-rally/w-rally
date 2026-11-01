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

# AMD FidelityFX / FSR SDK v2.4.0 (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v2.4.0; MIT, fidelityfx/
# LICENSE.md): the ffx-api headers, unchanged, under fidelityfx/ in the SDK's own layout (Kits/FidelityFX/api/include,
# Kits/FidelityFX/upscalers/include). The renderer's FSR upscaler (core/fsr.cpp) loads the AMD-signed loader DLL at run
# time, which loads the upscaler DLL (FSR 3.1, FSR 4 on RDNA 4) beside it. The two DLLs (58 MB) are not in git: they
# are fetched here once, pinned by SHA-256, into the build tree; without them the game runs with its own TAA only.
add_library(fidelityfx INTERFACE)
target_include_directories(fidelityfx INTERFACE fidelityfx/api/include fidelityfx/upscalers/include)
set(WRALLY_FFX_URL https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin)
set(WRALLY_FFX_DIR ${CMAKE_BINARY_DIR}/third_party/fidelityfx)
set(WRALLY_FFX_DLLS)
foreach(dll_hash amd_fidelityfx_loader_dx12.dll=e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb
                 amd_fidelityfx_upscaler_dx12.dll=aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace)
    string(REPLACE "=" ";" dll_hash ${dll_hash})
    list(GET dll_hash 0 dll)
    list(GET dll_hash 1 hash)
    set(path ${WRALLY_FFX_DIR}/${dll})
    if(EXISTS ${path})
        file(SHA256 ${path} have)
    endif()
    if(NOT EXISTS ${path} OR NOT have STREQUAL hash)
        message(STATUS "FidelityFX: fetching ${dll}")
        file(DOWNLOAD ${WRALLY_FFX_URL}/${dll} ${path} EXPECTED_HASH SHA256=${hash} STATUS st)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            message(WARNING "FidelityFX: ${dll} not fetched (${st}) - no FSR in this build")
            file(REMOVE ${path})
        endif()
    endif()
    if(EXISTS ${path})
        list(APPEND WRALLY_FFX_DLLS ${path})
    endif()
endforeach()
set(WRALLY_FFX_DLLS ${WRALLY_FFX_DLLS} CACHE INTERNAL "the FidelityFX DLLs fetched")
