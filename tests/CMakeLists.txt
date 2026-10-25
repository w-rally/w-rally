# wstub.dll: physics at the game's interface version, audio at one more (tests/stub-module/stub.cpp). Built beside the
# tests, not in bin/modules, so the game never lists it.
add_library(wstub SHARED stub-module/stub.cpp)
target_link_libraries(wstub PRIVATE wrally-iface)
target_compile_definitions(wstub PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(wstub PRIVATE /W4 /permissive-)
set_target_properties(wstub PROPERTIES PREFIX "")
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set_target_properties(wstub PROPERTIES RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/tests-bin)
endforeach()

# The handshake end to end: the compatible physics initialises and reports its info, the audio is listed as
# incompatible and left alone.
add_test(NAME module-handshake COMMAND module-info $<TARGET_FILE:wstub> --init --data ${CMAKE_BINARY_DIR}/tests-bin)
set_tests_properties(module-handshake PROPERTIES PASS_REGULAR_EXPRESSION
    "physics interface 1 - compatible\n +id stub, \"Stub physics \\(tests\\)\".*audio interface 2 - INCOMPATIBLE")

# A folder with no modules is not an error.
add_test(NAME module-empty-folder COMMAND module-info ${CMAKE_CURRENT_SOURCE_DIR}/stub-module)
set_tests_properties(module-empty-folder PROPERTIES PASS_REGULAR_EXPRESSION "no module DLLs found")
