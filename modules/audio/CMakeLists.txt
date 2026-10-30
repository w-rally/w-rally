# The sound (imported from plugin-rbr audio/, docs/imported.md): it listens to the car's records on the signal bus;
# XAudio2 for the device. game-audio is the library; WAudio.dll puts it behind AudioApi (phase 3).
add_library(game-audio STATIC src/audio/audio.cpp src/audio/bank.cpp src/audio/device.cpp src/audio/grains.cpp
            src/audio/mix.cpp src/audio/sample.cpp src/audio/files.cpp src/audio/voices.cpp)
target_include_directories(game-audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(game-audio PUBLIC wrally-signals xaudio2 PRIVATE stb-vorbis)
target_compile_definitions(game-audio PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(game-audio PRIVATE /W4)
set_target_properties(game-audio PROPERTIES CXX_STANDARD 17)

# WAudio.dll: the module (src/module/module.cpp) over game-audio, in bin/modules/audio.
add_library(WAudio SHARED src/module/module.cpp)
target_link_libraries(WAudio PRIVATE game-audio)
target_compile_definitions(WAudio PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(WAudio PRIVATE /W4)
set_target_properties(WAudio PROPERTIES CXX_STANDARD 17)
wrally_module_output(WAudio audio)
wrally_build_hash(WAudio WAUDIO_BUILD_HASH ${CMAKE_CURRENT_SOURCE_DIR}/src)
