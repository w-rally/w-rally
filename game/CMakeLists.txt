# WRally.exe. Phase 3: phys-drive (docs/imported.md) with its physics and its sound modules - WPhysicsRBR.dll and
# WAudio.dll, loaded from bin/modules through wrally::physics::Physics (src/physics) and wrally::sound::Sound
# (src/sound). Still linked in: RBR's file readers (phys-data); still drawing through ngg-api.dll.
add_executable(WRally src/drive/drive.cpp src/drive/car_model.cpp src/drive/replay_camera.cpp
               src/physics/physics.cpp src/rules/stage.cpp src/sound/sound.cpp)
target_include_directories(WRally PRIVATE src)
target_link_libraries(WRally PRIVATE phys-data wrally-modules wrally-signals ngg-api)
# Not linked: loaded at run time from bin/modules - but built with the game.
add_dependencies(WRally WPhysicsRBR WAudio)
# Where WRallyFile finds content/ and game/config/ when they are not beside the exe (a build run from its tree).
target_compile_definitions(WRally PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WRALLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(WRally PROPERTIES CXX_STANDARD 17)
