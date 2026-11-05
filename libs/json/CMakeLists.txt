# wrally-json: the small JSON and YAML readers for W-Rally's own manifests (car.yaml, track.yaml, physics.yaml; the
# older .json names too), shared by the game and the VFS (libs/vfs).
add_library(wrally-json STATIC src/json.cpp src/yaml.cpp)
target_include_directories(wrally-json PUBLIC include)
target_compile_options(wrally-json PRIVATE /W4 /permissive-)
