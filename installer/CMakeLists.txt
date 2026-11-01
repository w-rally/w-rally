# The installer (installer/W-Rally.wxs, built by installer/build-msi.ps1): what it ships - `cmake --install` lays the
# game out as installed (the exe, the modules, the game's config, the content) - and its custom actions' DLL.

install(TARGETS W-Rally RUNTIME DESTINATION .)
install(TARGETS WPhysicsRBR RUNTIME DESTINATION modules/physics)
install(TARGETS WAudio RUNTIME DESTINATION modules/audio)
install(TARGETS WRenderer RUNTIME DESTINATION modules/renderer)
install(FILES ${PROJECT_SOURCE_DIR}/third_party/dxc/dxcompiler.dll ${PROJECT_SOURCE_DIR}/third_party/dxc/dxil.dll
        DESTINATION modules/renderer)
# FSR: AMD's signed ffx-api DLLs (fetched at configure, third_party/CMakeLists.txt) and their licence.
if(WRALLY_FFX_DLLS)
    install(FILES ${WRALLY_FFX_DLLS} DESTINATION modules/renderer)
    install(FILES ${PROJECT_SOURCE_DIR}/third_party/fidelityfx/LICENSE.md DESTINATION modules/renderer
            RENAME FidelityFX-LICENSE.md)
endif()
install(FILES ${PROJECT_SOURCE_DIR}/game/config/sound-mix.ini DESTINATION game/config)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/content/ DESTINATION content PATTERN ".git*" EXCLUDE)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE.md ${PROJECT_SOURCE_DIR}/README.md DESTINATION .)

# The custom actions (ca/rbr_folder.cpp): beside the installer's build, not shipped.
add_library(wrally-installer-ca SHARED ca/rbr_folder.cpp)
target_link_libraries(wrally-installer-ca PRIVATE msi)
target_compile_definitions(wrally-installer-ca PRIVATE UNICODE _UNICODE NOMINMAX)
set_target_properties(wrally-installer-ca PROPERTIES OUTPUT_NAME RbrFolder)
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set_target_properties(wrally-installer-ca PROPERTIES RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/installer-ca)
endforeach()
