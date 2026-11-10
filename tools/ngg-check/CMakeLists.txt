# ngg-check: the .ngg writer (ngg-format) against its reader and the renderer's own loader, and W-Rally's legacy
# importer end to end (main.cpp says how). Builds the game's importer and renderer wrapper in.
set(GAME_SRC ${PROJECT_SOURCE_DIR}/game/src)
add_executable(ngg-check main.cpp placeholder_tree.cpp ${GAME_SRC}/assets/legacy_stage.cpp ${GAME_SRC}/assets/legacy_weather.cpp
               ${GAME_SRC}/formats/lbs_geometry.cpp ${GAME_SRC}/formats/track_settings.cpp ${GAME_SRC}/formats/dds_image.cpp ${GAME_SRC}/rules/stage.cpp
               ${GAME_SRC}/render/renderer.cpp)
target_include_directories(ngg-check PRIVATE ${GAME_SRC})
target_link_libraries(ngg-check PRIVATE ngg-format phys-data wrally-modules)
target_compile_definitions(ngg-check PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(ngg-check PRIVATE /W4)
set_target_properties(ngg-check PROPERTIES CXX_STANDARD 17)
add_dependencies(ngg-check WRenderer)
