# signals: the record bus the physics emits on and the audio, HUD and recorder listen to (headers only).
add_library(wrally-signals INTERFACE)
target_include_directories(wrally-signals INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
