# wrally_build_info(<target>): the game's version and the commit it was built from, for the corner of its screens -
# <build>/generated/build_info.h (WRALLY_VERSION, WRALLY_COMMIT), written on every build and touched only when it
# changes. The version: WRALLY_VERSION (CI passes 0.1.<run number>), else the project's. The commit: git's short hash,
# "+" when the tree has changes of its own; "" outside a git checkout.
#
# Run as a script (cmake -P) it writes the header: -DOUT= -DVERSION= -DSRC=.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(WRALLY_VERSION "${PROJECT_VERSION}" CACHE STRING "The version the game shows (CI: 0.1.<run number>)")
endif()

function(wrally_build_info target)
    set(out ${CMAKE_BINARY_DIR}/generated/build_info.h)
    add_custom_target(${target}-build-info
        COMMAND ${CMAKE_COMMAND} -DOUT=${out} -DVERSION=${WRALLY_VERSION} -DSRC=${PROJECT_SOURCE_DIR}
                -P ${PROJECT_SOURCE_DIR}/cmake/BuildInfo.cmake
        BYPRODUCTS ${out}
        COMMENT "the build's version and commit")
    add_dependencies(${target} ${target}-build-info)
    target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND OUT)
    set(commit "")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SRC} rev-parse --short HEAD
                        OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
        if(rc)
            set(commit "")
        else()
            execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SRC} status --porcelain --untracked-files=no --ignore-submodules=dirty
                            OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(changes)
                string(APPEND commit "+")
            endif()
        endif()
    endif()
    set(text "#pragma once\n// Written by cmake/BuildInfo.cmake on every build.\n#define WRALLY_VERSION \"${VERSION}\"\n#define WRALLY_COMMIT \"${commit}\"\n")
    set(old "")
    if(EXISTS ${OUT})
        file(READ ${OUT} old)
    endif()
    if(NOT old STREQUAL text)
        file(WRITE ${OUT} "${text}")
    endif()
endif()
