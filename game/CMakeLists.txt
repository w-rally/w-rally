# WRally.exe. Phase 4: phys-drive (docs/imported.md) on three modules - WPhysicsRBR.dll, WAudio.dll and WRenderer.dll,
# loaded from bin/modules through wrally::physics::Physics (src/physics), wrally::sound::Sound (src/sound) and
# wrally::render::Renderer (src/render); the HUD is Dear ImGui (src/ui), drawn by the renderer's UI pass. Still linked
# in: RBR's file readers (phys-data).
add_executable(WRally src/drive/drive.cpp src/drive/car_model.cpp src/drive/replay_camera.cpp
               src/physics/physics.cpp src/rules/stage.cpp src/sound/sound.cpp src/render/renderer.cpp
               src/ui/imgui_layer.cpp)
target_include_directories(WRally PRIVATE src)
target_link_libraries(WRally PRIVATE phys-data wrally-modules wrally-signals imgui)
# Not linked: loaded at run time from bin/modules - but built with the game.
add_dependencies(WRally WPhysicsRBR WAudio WRenderer)
# Where WRallyFile finds content/ and game/config/ when they are not beside the exe (a build run from its tree).
target_compile_definitions(WRally PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WRALLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(WRally PROPERTIES CXX_STANDARD 17)
# A window application (no console window of its own), still entered at main(): output goes where drive.cpp's
# SetUpOutput sends it.
set_target_properties(WRally PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(WRally PRIVATE /ENTRY:mainCRTStartup)
