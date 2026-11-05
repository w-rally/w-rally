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
    "physics interface [0-9]+ - compatible\n +id stub, \"Stub physics \\(tests\\)\".*audio interface [0-9]+ - INCOMPATIBLE")

# A folder with no modules is not an error.
add_test(NAME module-empty-folder COMMAND module-info ${CMAKE_CURRENT_SOURCE_DIR}/stub-module)
set_tests_properties(module-empty-folder PROPERTIES PASS_REGULAR_EXPRESSION "no module DLLs found")

# The real physics module answers the handshake and initialises.
add_test(NAME physics-rbr-handshake COMMAND module-info ${WRALLY_BIN}/modules/physics --init --data ${CMAKE_BINARY_DIR}/tests-bin)
set_tests_properties(physics-rbr-handshake PROPERTIES PASS_REGULAR_EXPRESSION
    "WPhysicsRBR.dll: physics interface [0-9]+ - compatible\n +id rbr, \"RBR original\"")

# The golden check (docs/design.md 4.2): RBR's own recording of a whole stage replayed through the game and
# WPhysicsRBR.dll, every pose bit-exact. Needs an RBR folder, stage 41's .ngg to draw (another map's would leave the
# car with no ground drawn under it) and the recording - this machine's paths by default; the test is left out where
# they are missing. It opens the game's window while it runs.
set(WRALLY_TEST_RBR_DIR "<RBR>" CACHE PATH "RBR folder for the golden replay test")
set(WRALLY_TEST_NGG "<phys-data>/m5/track-41.ngg" CACHE FILEPATH "stage 41's .ngg (drawn only)")
set(WRALLY_TEST_GOLDEN "<phys-data>/m2/run-41-20261011-150633.phr" CACHE FILEPATH "RBR's recording of stage 41")
if(EXISTS ${WRALLY_TEST_RBR_DIR}/Maps/track-41_M.trk AND EXISTS ${WRALLY_TEST_NGG} AND EXISTS ${WRALLY_TEST_GOLDEN})
    add_test(NAME golden-replay-41 COMMAND W-Rally ${WRALLY_TEST_RBR_DIR} ${WRALLY_TEST_NGG} --stage track-41_M --step 0.1
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

# The renderer module answers the handshake (not initialised: a renderer needs a window - the golden replay above
# runs it for real).
add_test(NAME renderer-handshake COMMAND module-info ${WRALLY_BIN}/modules/renderer)
set_tests_properties(renderer-handshake PROPERTIES PASS_REGULAR_EXPRESSION
    "WRenderer.dll: renderer interface [0-9]+ - compatible")

# A car's part drawn where the physics has it (game/src/assets/car_render.cpp's PartNode): at rest exactly the body,
# and torn off it does not jump (the world pose of a moment gives the in-car pose's node).
set(GAME_SRC ${PROJECT_SOURCE_DIR}/game/src)
add_executable(car-parts car_parts.cpp ${GAME_SRC}/assets/car_render.cpp ${GAME_SRC}/assets/car_model.cpp
               ${GAME_SRC}/render/renderer.cpp)
target_include_directories(car-parts PRIVATE ${GAME_SRC})
target_link_libraries(car-parts PRIVATE phys-data wrally-modules)
target_compile_definitions(car-parts PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
set_target_properties(car-parts PROPERTIES CXX_STANDARD 17)
add_test(NAME car-parts COMMAND car-parts)

# The renderer's sky clockwork (modules/renderer/src/core/astronomy.*, NGG design 5.4 N1) against Meeus's worked
# examples and the solstice's noon sun.
add_executable(astronomy-check astronomy_check.cpp ${PROJECT_SOURCE_DIR}/modules/renderer/src/core/astronomy.cpp)
target_include_directories(astronomy-check PRIVATE ${PROJECT_SOURCE_DIR}/modules/renderer/src/core)
set_target_properties(astronomy-check PROPERTIES CXX_STANDARD 17)
add_test(NAME astronomy-check COMMAND astronomy-check)
set_tests_properties(astronomy-check PROPERTIES PASS_REGULAR_EXPRESSION "astronomy-check: all good")

# What the Quick rally menu offers (game/src/assets/catalog.cpp) from the RBR folder: every stage, those no .ngg draws
# imported from their .lbs with drawn weathers, no protected (encrypted) one, and a map under 10's weathers under their
# two-digit names.
add_executable(catalog-check catalog_check.cpp ${GAME_SRC}/assets/catalog.cpp ${GAME_SRC}/assets/car_register.cpp
               ${GAME_SRC}/assets/track_register.cpp ${GAME_SRC}/app/settings.cpp
               ${GAME_SRC}/app/options.cpp ${GAME_SRC}/input/bindings.cpp ${GAME_SRC}/rules/stage.cpp
               ${GAME_SRC}/formats/track_settings.cpp ${GAME_SRC}/formats/lbs_geometry.cpp)
target_include_directories(catalog-check PRIVATE ${GAME_SRC})
target_link_libraries(catalog-check PRIVATE wrally-json phys-data wrally-modules wrally-signals)
target_compile_definitions(catalog-check PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WRALLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(catalog-check PROPERTIES CXX_STANDARD 17)
if(EXISTS ${WRALLY_TEST_RBR_DIR}/Maps/track-4_E.lbs)
    add_test(NAME catalog-every-stage COMMAND catalog-check ${WRALLY_TEST_RBR_DIR})
    set_tests_properties(catalog-every-stage PROPERTIES PASS_REGULAR_EXPRESSION "catalog-check: all good")
endif()

# The .ngg writer and its readers in step (tools/ngg-check, docs/design.md decision 2): a made-up scene written, read
# back the same and loaded by the renderer; Wallaby's stage 41 .ngg (another producer) read, rewritten the same and
# both loaded; W-Rally's legacy importer on RBR's stage 41 .lbs, loaded by the renderer. They open a hidden window.
add_test(NAME ngg-roundtrip COMMAND ngg-check)
set_tests_properties(ngg-roundtrip PROPERTIES PASS_REGULAR_EXPRESSION "ngg-check: all good")
if(EXISTS ${WRALLY_TEST_NGG})
    add_test(NAME ngg-producer-wallaby COMMAND ngg-check ${WRALLY_TEST_NGG})
    set_tests_properties(ngg-producer-wallaby PROPERTIES PASS_REGULAR_EXPRESSION "ngg-check: all good")
endif()
if(EXISTS ${WRALLY_TEST_RBR_DIR}/Maps/track-41_M.lbs)
    add_test(NAME legacy-import-41 COMMAND ngg-check --lbs ${WRALLY_TEST_RBR_DIR}/Maps/track-41_M.lbs)
    set_tests_properties(legacy-import-41 PROPERTIES PASS_REGULAR_EXPRESSION
        "weather: 7 presets from TrackSettings.ini, default 41M_crisp_clear .*imported track-41_M.lbs: .* 304744 triangles, .*\(0 missing\).*ngg-check: all good")
endif()
# Its weathers on a map under 10: TrackSettings.ini writes it "04", so a "4" prefix would find none.
if(EXISTS ${WRALLY_TEST_RBR_DIR}/Maps/track-4_E.lbs)
    add_test(NAME legacy-import-weather-4 COMMAND ngg-check --lbs ${WRALLY_TEST_RBR_DIR}/Maps/track-4_E.lbs)
    set_tests_properties(legacy-import-weather-4 PROPERTIES PASS_REGULAR_EXPRESSION
        "weather: 43 presets from TrackSettings.ini, default 04E_crisp_clear .*ngg-check: all good")
endif()
# A protected (encrypted) stage is refused whole, never imported in part (NGG design 3.7).
if(EXISTS ${WRALLY_TEST_RBR_DIR}/Maps/track-96_E.lbs)
    add_test(NAME legacy-import-protected COMMAND ngg-check --lbs ${WRALLY_TEST_RBR_DIR}/Maps/track-96_E.lbs)
    set_tests_properties(legacy-import-protected PROPERTIES PASS_REGULAR_EXPRESSION
        "encrypted - a protected stage \\(block \\(1,[0-9]\\) version 8[89]\\), not imported")
endif()

# The physics' libraries depend downwards only (docs/design.md 4.3a): each linked WHOLE into an empty program with only
# the libraries below it - a call into one above fails the link, so the build itself is the check.
foreach(layer rbr-collision rbr-tyre rbr-world rbr-car)
    add_executable(physics-layer-${layer} physics-layers/main.cpp)
    target_link_libraries(physics-layer-${layer} PRIVATE ${layer})
    target_link_options(physics-layer-${layer} PRIVATE /WHOLEARCHIVE:$<TARGET_FILE:${layer}>)
    foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
        set_target_properties(physics-layer-${layer} PROPERTIES RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/tests-bin)
    endforeach()
    add_test(NAME physics-layer-${layer} COMMAND physics-layer-${layer})
endforeach()

# The co-driver on stage 41's pacenotes without a voice (tests/codriver.cpp): the HUD's calls as they are called, and
# nothing called before the stage time runs.
add_executable(codriver codriver.cpp ${GAME_SRC}/sound/codriver.cpp ${GAME_SRC}/sound/sound.cpp)
target_include_directories(codriver PRIVATE ${GAME_SRC})
target_link_libraries(codriver PRIVATE phys-data wrally-modules wrally-signals)
target_compile_definitions(codriver PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
set_target_properties(codriver PROPERTIES CXX_STANDARD 17)
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set_target_properties(codriver PROPERTIES RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/tests-bin)
endforeach()
if(EXISTS ${WRALLY_TEST_RBR_DIR}/Maps/track-41_M.dls)
    add_test(NAME codriver-41 COMMAND codriver ${WRALLY_TEST_RBR_DIR}/Maps/track-41_M.dls)
    set_tests_properties(codriver-41 PROPERTIES PASS_REGULAR_EXPRESSION "codriver: [0-9]+ calls; all good")
endif()

# The Debug menu's sound mix (game/src/sound/mix_edit.cpp): what it sends and saves is what the audio module reads
# (audio::Mix), and saving keeps the mix file's comments.
add_executable(mix-edit mix_edit.cpp ${GAME_SRC}/sound/mix_edit.cpp)
target_include_directories(mix-edit PRIVATE ${GAME_SRC})
target_link_libraries(mix-edit PRIVATE game-audio)
set_target_properties(mix-edit PROPERTIES CXX_STANDARD 17)
add_test(NAME mix-edit COMMAND mix-edit ${PROJECT_SOURCE_DIR}/game/config/sound-mix.ini ${CMAKE_BINARY_DIR}/tests-bin/mix-edit.ini)
set_tests_properties(mix-edit PROPERTIES PASS_REGULAR_EXPRESSION "mix-edit: all good")

# Where a file is found (libs/vfs, docs/content-layers.md): a made-up downloaded layer, W-Rally layer and RBR folder
# (with a physics.rbz) - versions newest first, patches over their bases and over RBR's items, RBR's maps and cars
# mapped as items, physics.rbz's physics unpacked under the cars, writes only into the first layer; then the real RBR
# folder where there is one.
add_executable(vfs-check vfs_check.cpp)
target_link_libraries(vfs-check PRIVATE wrally-vfs)
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set_target_properties(vfs-check PROPERTIES RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/tests-bin)
endforeach()
add_test(NAME vfs-made-up COMMAND vfs-check ${CMAKE_BINARY_DIR}/tests-bin/vfs-scratch)
set_tests_properties(vfs-made-up PROPERTIES PASS_REGULAR_EXPRESSION "vfs-check: all good")
if(EXISTS ${WRALLY_TEST_RBR_DIR}/physics.rbz)
    add_test(NAME vfs-rbr COMMAND vfs-check ${CMAKE_BINARY_DIR}/tests-bin/vfs-scratch-rbr ${WRALLY_TEST_RBR_DIR})
    set_tests_properties(vfs-rbr PROPERTIES PASS_REGULAR_EXPRESSION "vfs-check: all good")
endif()
