# The native RBR physics (imported from plugin-rbr phys/, docs/imported.md). Phase 1: static libraries the game links;
# phase 2 puts them behind PhysicsApi as WPhysicsRBR.dll.
#
# The compile flags are part of the physics: it is bit-exact with RBR only while every float op compiles to one IEEE
# single op in source order (precise FP, no FMA contraction) - kept exactly as plugin-rbr builds it, C++17 included.

# rsqrtss definition (num/approx.h): a fixed table captured on AMD or Intel - the same bits on every machine - or NATIVE,
# the running CPU's instruction. plugin-rbr's tests run with AMD.
set(PHYS_RSQRT "AMD" CACHE STRING "rsqrtss definition: AMD | INTEL | NATIVE")
set_property(CACHE PHYS_RSQRT PROPERTY STRINGS AMD INTEL NATIVE)
if(NOT PHYS_RSQRT MATCHES "^(AMD|INTEL|NATIVE)$")
    message(FATAL_ERROR "PHYS_RSQRT must be AMD, INTEL or NATIVE (got '${PHYS_RSQRT}')")
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# phys-num: the game's float arithmetic emulated bit for bit, the vector / matrix / quaternion and collision primitives.
add_library(phys-num STATIC src/num/approx.cpp src/num/trig.cpp src/num/math.cpp src/num/collide.cpp src/num/curves.cpp
            src/num/warn.cpp)
target_include_directories(phys-num PUBLIC ${SRC})
target_compile_definitions(phys-num PUBLIC PHYS_RSQRT_${PHYS_RSQRT})
target_compile_options(phys-num PUBLIC /fp:precise PRIVATE /W4)

# phys-data: readers of RBR's files (.trk .col .lsp .rbz .rpl .phr .lbs .sgc .dls).
add_library(phys-data STATIC src/data/rpl.cpp src/data/trk.cpp src/data/trkworld.cpp src/data/lsp.cpp src/data/inflate.cpp
            src/data/rec.cpp src/data/lbs.cpp src/data/sgc.cpp src/data/dls.cpp src/data/files.cpp)
target_include_directories(phys-data PUBLIC ${SRC})

# phys-native: the physics as native classes.
add_library(phys-native STATIC src/native/stage_collision.cpp src/native/rigid_body.cpp src/native/entity.cpp
            src/native/collision_mesh.cpp src/native/event.cpp src/native/contacts.cpp src/native/brake_wall.cpp
            src/native/world.cpp src/native/solver.cpp src/native/tyre.cpp src/native/suspension.cpp src/native/wheel.cpp
            src/native/drive.cpp src/native/engine.cpp src/native/vcu.cpp src/native/car.cpp src/native/driveline.cpp
            src/native/hinge.cpp src/native/damage.cpp src/native/def_load.cpp src/native/setup.cpp src/native/load.cpp
            src/native/run.cpp)
target_link_libraries(phys-native PUBLIC phys-num phys-data)
target_compile_options(phys-native PRIVATE /W4)

# phys-sim: sim/sim.h's Physics over the native world (NativeSim).
add_library(phys-sim STATIC src/sim/native_sim.cpp src/sim/fault.cpp)
target_link_libraries(phys-sim PUBLIC phys-native wrally-signals)
target_compile_definitions(phys-sim PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(phys-sim PRIVATE /W4)

foreach(t phys-num phys-data phys-native phys-sim)
    set_target_properties(${t} PROPERTIES CXX_STANDARD 17)
endforeach()

# WPhysicsRBR.dll: the module (src/module/module.cpp) over phys-sim, in bin/modules/physics.
add_library(WPhysicsRBR SHARED src/module/module.cpp)
target_link_libraries(WPhysicsRBR PRIVATE phys-sim)
target_compile_definitions(WPhysicsRBR PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(WPhysicsRBR PRIVATE /W4)
set_target_properties(WPhysicsRBR PROPERTIES CXX_STANDARD 17)
wrally_module_output(WPhysicsRBR physics)
# The rsqrt table changes the arithmetic, so it is part of the hash.
wrally_build_hash(WPhysicsRBR WPHYSICS_RBR_BUILD_HASH ${SRC} EXTRA "PHYS_RSQRT=${PHYS_RSQRT}")
