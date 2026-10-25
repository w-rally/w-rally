add_executable(module-info main.cpp)
target_link_libraries(module-info PRIVATE wrally-modules)
target_compile_options(module-info PRIVATE /W4 /permissive-)
target_compile_definitions(module-info PRIVATE _CRT_SECURE_NO_WARNINGS)
