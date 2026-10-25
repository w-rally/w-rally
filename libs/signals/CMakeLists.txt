# signals: the record bus the physics emits on and the audio, HUD and recorder listen to (headers only).
add_library(wrally-signals INTERFACE)
target_include_directories(wrally-signals INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# signals/convert.h maps the records to the module interface's (wrally/iface/records.h).
target_link_libraries(wrally-signals INTERFACE wrally-iface)
