# The net module (WNet.dll) is not built here: it comes from the private w-rally/wnet repo, as a signed GitHub Release
# pinned in cmake/wnet.lock (version, SHA-256, the release signing key). The result: WRALLY_WNET_MODULE, the DLL's path
# ("" none - the game then runs offline), which the game's build copies to bin/modules/net and the installer ships.
#
#   WRALLY_WNET_DLL=<path>   a WNet.dll of your own (a build of the wnet repo): used as it is, nothing fetched
#   WRALLY_FETCH_WNET=ON     (the default) else the pinned release, downloaded and verified by
#                            tools/fetch-wnet/fetch_wnet.py into <build>/wnet at configure time. The repo is private:
#                            it needs a token (WNET_GITHUB_TOKEN, GH_TOKEN, or `gh auth login`); none, no net module.
#
# A download that fails is a warning (offline build); one that does not verify stops the configure - an unverified
# WNet.dll is never used.
set(WRALLY_WNET_DLL "" CACHE FILEPATH "A WNet.dll of your own for bin/modules/net (a build of the wnet repo); empty: the pinned release")
option(WRALLY_FETCH_WNET "Download the pinned, verified WNet.dll (cmake/wnet.lock) when WRALLY_WNET_DLL is not set" ON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wnet.lock)

set(WRALLY_WNET_MODULE "")
if(WRALLY_WNET_DLL)
    set(WRALLY_WNET_MODULE ${WRALLY_WNET_DLL})
    message(STATUS "WNet.dll: ${WRALLY_WNET_DLL} (WRALLY_WNET_DLL)")
elseif(WRALLY_FETCH_WNET)
    find_package(Python3 3.8 COMPONENTS Interpreter)
    if(NOT Python3_FOUND)
        message(WARNING "WNet.dll: no Python to fetch it with - building without the net module")
    else()
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fetch-wnet/fetch_wnet.py ${CMAKE_BINARY_DIR}/wnet
            RESULT_VARIABLE _wnet_rc OUTPUT_VARIABLE _wnet_out ERROR_VARIABLE _wnet_err
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
        if(_wnet_rc EQUAL 0)
            set(WRALLY_WNET_MODULE ${CMAKE_BINARY_DIR}/wnet/WNet.dll)
            message(STATUS "${_wnet_out}")
        elseif(_wnet_rc EQUAL 2)
            message(STATUS "${_wnet_err}")
        elseif(_wnet_rc EQUAL 3)
            message(FATAL_ERROR "${_wnet_err}")
        else()
            message(WARNING "${_wnet_err}${_wnet_out} - building without the net module")
        endif()
    endif()
endif()
