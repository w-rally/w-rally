# car-uv-dump: a car's exterior mesh groups as UV triangles - its texture file and shader class each - for
# make_pbr.py, which drafts the car's PBR maps from them (README in make_pbr.py).
set(GAME_SRC ${PROJECT_SOURCE_DIR}/game/src)
add_executable(car-uv-dump uv_dump.cpp ${GAME_SRC}/assets/car_model.cpp)
target_include_directories(car-uv-dump PRIVATE ${GAME_SRC})
target_link_libraries(car-uv-dump PRIVATE phys-data)
target_compile_definitions(car-uv-dump PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
set_target_properties(car-uv-dump PROPERTIES CXX_STANDARD 17)
