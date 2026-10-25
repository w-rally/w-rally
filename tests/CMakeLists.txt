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
    "physics interface 1 - compatible\n +id stub, \"Stub physics \\(tests\\)\".*audio interface [0-9]+ - INCOMPATIBLE")

# A folder with no modules is not an error.
add_test(NAME module-empty-folder COMMAND module-info ${CMAKE_CURRENT_SOURCE_DIR}/stub-module)
set_tests_properties(module-empty-folder PROPERTIES PASS_REGULAR_EXPRESSION "no module DLLs found")

# The real physics module answers the handshake and initialises.
add_test(NAME physics-rbr-handshake COMMAND module-info ${WRALLY_BIN}/modules/physics --init --data ${CMAKE_BINARY_DIR}/tests-bin)
set_tests_properties(physics-rbr-handshake PROPERTIES PASS_REGULAR_EXPRESSION
    "WPhysicsRBR.dll: physics interface 1 - compatible\n +id rbr, \"RBR original\"")

# The golden check (docs/design.md 4.2): RBR's own recording of a whole stage replayed through the game and
# WPhysicsRBR.dll, every pose bit-exact. Needs an RBR folder, a stage .ngg to draw and the recording - this machine's
# paths by default; the test is left out where they are missing. It opens the game's window while it runs.
set(WRALLY_TEST_RBR_DIR "C:/Brb" CACHE PATH "RBR folder for the golden replay test")
set(WRALLY_TEST_NGG "C:/Brb/Ngg/track-42.ngg" CACHE FILEPATH "any stage .ngg (drawn only)")
set(WRALLY_TEST_GOLDEN "C:/Projects/phys-data/m2/run-41-20261011-150633.phr" CACHE FILEPATH "RBR's recording of stage 41")
if(EXISTS ${WRALLY_TEST_RBR_DIR}/Maps/track-41_M.trk AND EXISTS ${WRALLY_TEST_NGG} AND EXISTS ${WRALLY_TEST_GOLDEN})
    add_test(NAME golden-replay-41 COMMAND WRally ${WRALLY_TEST_RBR_DIR} ${WRALLY_TEST_NGG} --stage track-41_M --step 0.1
             --no-sound --overrides none --replay ${WRALLY_TEST_GOLDEN} --out ${CMAKE_BINARY_DIR}/tests-bin/golden-41.phr)
    set_tests_properties(golden-replay-41 PROPERTIES PASS_REGULAR_EXPRESSION "replay check: [0-9]+ poses, all bit-exact"
                         TIMEOUT 600)
else()
    message(STATUS "golden-replay-41 left out: no ${WRALLY_TEST_RBR_DIR}, ${WRALLY_TEST_NGG} or ${WRALLY_TEST_GOLDEN}")
endif()

# The sound through WAudio.dll against the audio library linked in, offline from the same records, bit for bit
# (tools/audio-check): made-up records (no data needed), then the same with a mix away from the defaults (setCarMix).
add_test(NAME audio-offline-synthetic COMMAND audio-check)
add_test(NAME audio-offline-synthetic-mix COMMAND audio-check --mix ${CMAKE_CURRENT_SOURCE_DIR}/data/audio-mix.ini)
set_tests_properties(audio-offline-synthetic audio-offline-synthetic-mix PROPERTIES PASS_REGULAR_EXPRESSION
    "the same, bit for bit")
# RBR's sounds and the physics' records of RBR's stage-41 recording (its first 20000 ticks), where the data is.
if(EXISTS ${WRALLY_TEST_RBR_DIR}/Maps/track-41_M.trk AND EXISTS ${WRALLY_TEST_RBR_DIR}/audio AND EXISTS ${WRALLY_TEST_GOLDEN})
    add_test(NAME audio-offline-replay-41 COMMAND audio-check --game ${WRALLY_TEST_RBR_DIR} --stage track-41_M
             --replay ${WRALLY_TEST_GOLDEN} --ticks 20000 --mix ${CMAKE_CURRENT_SOURCE_DIR}/data/audio-mix.ini)
    set_tests_properties(audio-offline-replay-41 PROPERTIES PASS_REGULAR_EXPRESSION "the same, bit for bit" TIMEOUT 600)
endif()

# The real audio module answers the handshake and initialises (offline: no device).
add_test(NAME audio-handshake COMMAND module-info ${WRALLY_BIN}/modules/audio --init --data ${CMAKE_BINARY_DIR}/tests-bin)
set_tests_properties(audio-handshake PROPERTIES PASS_REGULAR_EXPRESSION
    "WAudio.dll: audio interface [0-9]+ - compatible\n +id waudio, \"WAudio\"")
