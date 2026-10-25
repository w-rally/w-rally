# WRally.exe. Phase 1: phys-drive (docs/imported.md) linked against the imported physics and audio libraries, drawing
# through ngg-api.dll.
add_executable(WRally src/drive/drive.cpp src/drive/car_model.cpp src/drive/replay_camera.cpp)
target_include_directories(WRally PRIVATE src)
target_link_libraries(WRally PRIVATE phys-sim game-audio ngg-api)
# Where WRallyFile finds content/ and game/config/ when they are not beside the exe (a build run from its tree).
target_compile_definitions(WRally PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WRALLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(WRally PROPERTIES CXX_STANDARD 17)
