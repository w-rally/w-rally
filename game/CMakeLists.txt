# WRally.exe (docs/design.md 6.5): the game on three modules - WPhysicsRBR.dll, WAudio.dll and WRenderer.dll, loaded
# from bin/modules. Its parts:
#   app/      main, options, settings, the window, menus and frame loop  wrally::app
#   session/  one stage run: physics, clock, driver layer, recording       wrally::session
#   camera/   the chase, bonnet, above, beside and TV cameras              wrally::camera
#   input/    the keys bound to the actions (Settings -> Controls)         wrally::input
#   rules/    the stage clock, a stage's surface and tyres                 wrally::rules
#   assets/   the content catalog, the legacy stage importer, car models,  wrally::assets
#             the fences
#   formats/  RBR's .lbs render geometry, .fnc fences                     wrally::formats
#   render/   the renderer module's wrapper, the stage objects             wrally::render
#   sound/    the audio module's wrapper                                   wrally::sound
#   physics/  the physics module's wrapper                                 wrally::physics
#   ui/       Dear ImGui, the menus and the HUD                            wrally::ui
#   core/     the small 3D math                                            wrally::math
# Still linked in: RBR's file readers (phys-data).
add_executable(WRally
    src/app/main.cpp src/app/options.cpp src/app/settings.cpp src/app/window.cpp src/app/game.cpp
    src/session/session.cpp src/session/sim_thread.cpp src/input/bindings.cpp
    src/camera/camera.cpp src/camera/replay_camera.cpp
    src/rules/stage.cpp
    src/assets/car_model.cpp src/assets/car_render.cpp src/assets/catalog.cpp src/assets/fences.cpp src/assets/legacy_stage.cpp src/assets/legacy_weather.cpp
    src/formats/lbs_geometry.cpp src/formats/fnc.cpp src/formats/track_settings.cpp
    src/render/renderer.cpp src/render/stage_objects.cpp
    src/sound/sound.cpp
    src/physics/physics.cpp
    src/ui/imgui_layer.cpp src/ui/hud.cpp src/ui/menus.cpp)
target_include_directories(WRally PRIVATE src)
target_link_libraries(WRally PRIVATE phys-data ngg-format wrally-modules wrally-signals imgui)
# Not linked: loaded at run time from bin/modules - but built with the game.
add_dependencies(WRally WPhysicsRBR WAudio WRenderer)
# Where WRallyFile finds content/ and game/config/ when they are not beside the exe (a build run from its tree).
target_compile_definitions(WRally PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WRALLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(WRally PRIVATE /W4)
set_target_properties(WRally PROPERTIES CXX_STANDARD 17)
# A window application (no console window of its own), still entered at main(): output goes where app/main.cpp's
# SetUpOutput sends it.
set_target_properties(WRally PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(WRally PRIVATE /ENTRY:mainCRTStartup)
