# wrally_build_hash(<target> <define> <source dir>): ModuleInfo::buildHash for a module - what replays and results record
# to say "this module, exactly": the sources under <source dir>, the compiler and `extra` (switches that change the
# results), hashed at configure time (a source change re-runs it through CONFIGURE_DEPENDS). Defines <define> as the
# first 64 bits, a ull literal.
function(wrally_build_hash target define dir)
    cmake_parse_arguments(ARG "" "EXTRA" "" ${ARGN})
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${dir}/*.cpp ${dir}/*.h ${dir}/*.inc ${dir}/*.inl)
    list(SORT sources)
    set(input "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${ARG_EXTRA}")
    foreach(f ${sources})
        file(SHA256 ${f} h)
        string(APPEND input " ${h}")
    endforeach()
    string(SHA256 hash "${input}")
    string(SUBSTRING ${hash} 0 16 hash)
    target_compile_definitions(${target} PRIVATE ${define}=0x${hash}ull)
    message(STATUS "${target} build hash ${hash}")
endfunction()

# wrally_module_output(<target> <kind folder>): the DLL into bin/modules/<kind folder>, as the game looks for it.
function(wrally_module_output target folder)
    foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY${cfg} ${WRALLY_BIN}/modules/${folder})
    endforeach()
endfunction()
