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

# NVIDIA DLSS SDK v310.9.16 (github.com/NVIDIA/DLSS, tag v310.9.16; NVIDIA RTX SDKs licence, nvidia-dlss/README.md):
# DLSS Super Resolution for the renderer's second upscaler (core/dlss.cpp). Unlike FidelityFX's, NVIDIA's headers are
# proprietary and may not be in this repository, so all of it is fetched here once, pinned by SHA-256, into the build
# tree: the NGX headers, the NGX loader library (linked; it finds the driver's NGX at run time) and the DLSS runtime
# nvngx_dlss.dll (59 MB, loaded by NGX at run time from beside WRenderer.dll), with the licence. Without any of them,
# or on a machine without an NVIDIA GPU, the game runs with no DLSS.
set(WRALLY_DLSS_URL https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16)
set(WRALLY_DLSS_DIR ${CMAKE_BINARY_DIR}/third_party/dlss)
set(WRALLY_DLSS_OK ON)
foreach(file_hash include/nvsdk_ngx.h=dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848
                  include/nvsdk_ngx_defs.h=ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e
                  include/nvsdk_ngx_params.h=943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc
                  include/nvsdk_ngx_helpers.h=5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471
                  include/nvsdk_ngx_helpers_d3d.h=ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba
                  include/nvsdk_ngx_helpers_cuda.h=f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042
                  lib/Windows_x86_64/x64/nvsdk_ngx_s.lib=7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765
                  lib/Windows_x86_64/rel/nvngx_dlss.dll=d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e
                  LICENSE.txt=d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0)
    string(REPLACE "=" ";" file_hash ${file_hash})
    list(GET file_hash 0 file)
    list(GET file_hash 1 hash)
    set(path ${WRALLY_DLSS_DIR}/${file})
    if(EXISTS ${path})
        file(SHA256 ${path} have)
    endif()
    if(NOT EXISTS ${path} OR NOT have STREQUAL hash)
        message(STATUS "DLSS: fetching ${file}")
        file(DOWNLOAD ${WRALLY_DLSS_URL}/${file} ${path} EXPECTED_HASH SHA256=${hash} STATUS st)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            message(WARNING "DLSS: ${file} not fetched (${st}) - no DLSS in this build")
            file(REMOVE ${path})
        endif()
    endif()
    if(NOT EXISTS ${path})
        set(WRALLY_DLSS_OK OFF)
    endif()
endforeach()
if(WRALLY_DLSS_OK)
    add_library(nvidia-dlss INTERFACE)
    target_include_directories(nvidia-dlss INTERFACE ${WRALLY_DLSS_DIR}/include)
    target_link_libraries(nvidia-dlss INTERFACE ${WRALLY_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib version)
    set(WRALLY_DLSS_DLLS ${WRALLY_DLSS_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll CACHE INTERNAL "the DLSS runtime fetched")
    set(WRALLY_DLSS_LICENSE ${WRALLY_DLSS_DIR}/LICENSE.txt CACHE INTERNAL "the DLSS SDK's licence")
else()
    set(WRALLY_DLSS_DLLS "" CACHE INTERNAL "the DLSS runtime fetched")
    set(WRALLY_DLSS_LICENSE "" CACHE INTERNAL "the DLSS SDK's licence")
endif()
