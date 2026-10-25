# The sound (imported from plugin-rbr audio/, docs/imported.md): it listens to the car's records on the signal bus;
# XAudio2 for the device. Phase 1: a static library the game links; phase 3 puts it behind AudioApi as WAudio.dll.
add_library(game-audio STATIC src/audio/audio.cpp src/audio/bank.cpp src/audio/device.cpp src/audio/grains.cpp
            src/audio/mix.cpp src/audio/sample.cpp)
target_include_directories(game-audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(game-audio PUBLIC wrally-signals xaudio2)
target_compile_definitions(game-audio PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(game-audio PRIVATE /W4)
set_target_properties(game-audio PROPERTIES CXX_STANDARD 17)
