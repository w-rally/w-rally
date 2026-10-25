# audio-check: WAudio.dll against the linked audio library, offline, the same records (main.cpp). Uses the game's own
# module wrappers, so it checks the path the game takes.
set(GAME_SRC ${PROJECT_SOURCE_DIR}/game/src)
add_executable(audio-check main.cpp ${GAME_SRC}/physics/physics.cpp ${GAME_SRC}/sound/sound.cpp ${GAME_SRC}/rules/stage.cpp)
target_include_directories(audio-check PRIVATE ${GAME_SRC})
target_link_libraries(audio-check PRIVATE game-audio phys-data wrally-modules wrally-signals)
add_dependencies(audio-check WAudio WPhysicsRBR)
target_compile_definitions(audio-check PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
set_target_properties(audio-check PROPERTIES CXX_STANDARD 17)
