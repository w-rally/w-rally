# WRally.exe. Phase 2: phys-drive (docs/imported.md) with its physics a module - WPhysicsRBR.dll, loaded from
# bin/modules/physics through wrally::physics::Physics (src/physics). Still linked in: the audio (game-audio) and
# RBR's file readers (phys-data); still drawing through ngg-api.dll.
add_executable(WRally src/drive/drive.cpp src/drive/car_model.cpp src/drive/replay_camera.cpp
               src/physics/physics.cpp src/rules/stage.cpp)
target_include_directories(WRally PRIVATE src)
target_link_libraries(WRally PRIVATE phys-data wrally-modules wrally-signals game-audio ngg-api)
# Not linked: loaded at run time from bin/modules - but built with the game.
add_dependencies(WRally WPhysicsRBR)
# Where WRallyFile finds content/ and game/config/ when they are not beside the exe (a build run from its tree).
target_compile_definitions(WRally PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WRALLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(WRally PROPERTIES CXX_STANDARD 17)
