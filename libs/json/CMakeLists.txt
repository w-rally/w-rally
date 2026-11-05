# wrally-json: the small JSON reader for W-Rally's own manifests (car.json, track.json, physics.json), shared by the
# game and the VFS (libs/vfs).
add_library(wrally-json STATIC src/json.cpp)
target_include_directories(wrally-json PUBLIC include)
target_compile_options(wrally-json PRIVATE /W4 /permissive-)
