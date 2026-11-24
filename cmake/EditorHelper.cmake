# Install step run after Qt's deploy script, before PruneBundle.cmake signs
# the bundle: makes the Neovim (Konsoll) helper bundle
# (Contents/Helpers/<EDITOR_BUNDLE_NAME>.app, EditorMode) run from the
# deployed bundle.
#
# Expects APP_BUNDLE (path to konsoll.app) and EDITOR_BUNDLE_NAME.
#
# - The helper's executable becomes a copy of the deployed one, whose Qt
#   references macdeployqt rewrote to @loader_path/../Frameworks: from the
#   helper's Contents/MacOS they point four levels up, to the outer bundle's
#   Frameworks (@rpath references, Sparkle's, resolve through the
#   @executable_path/../../../../Frameworks rpath the link adds).
# - qt.conf points Qt at the outer bundle's plugins and QML imports (a
#   prefix relative to the helper's Contents).

set(contents "${APP_BUNDLE}/Contents")
set(helper "${contents}/Helpers/${EDITOR_BUNDLE_NAME}.app/Contents")
if(NOT EXISTS "${helper}/Info.plist")
    message(FATAL_ERROR "The editor bundle is missing: ${helper}")
endif()

set(binary "${helper}/MacOS/konsoll")
file(COPY_FILE "${contents}/MacOS/konsoll" "${binary}")

execute_process(
    COMMAND otool -L "${binary}"
    OUTPUT_VARIABLE libraries
    RESULT_VARIABLE otool_result
)
if(NOT otool_result EQUAL 0)
    message(FATAL_ERROR "otool failed for ${binary}")
endif()
string(REGEX MATCHALL "@loader_path/\\.\\./Frameworks/[^ \t\n]+" references "${libraries}")
set(change_args)
foreach(reference IN LISTS references)
    string(REPLACE "@loader_path/../Frameworks/" "@loader_path/../../../../Frameworks/" moved "${reference}")
    list(APPEND change_args -change "${reference}" "${moved}")
endforeach()
if(change_args)
    execute_process(
        COMMAND install_name_tool ${change_args} "${binary}"
        RESULT_VARIABLE change_result
        ERROR_VARIABLE change_error
    )
    # install_name_tool warns that the signature is invalidated: the bundle
    # is signed again next.
    if(NOT change_result EQUAL 0)
        message(FATAL_ERROR "install_name_tool failed for ${binary}: ${change_error}")
    endif()
endif()

file(WRITE "${helper}/Resources/qt.conf"
    "[Paths]\nPrefix = ../../..\nPlugins = PlugIns\nQmlImports = Resources/qml\n"
)
message(STATUS "Editor bundle linked to the outer bundle: ${helper}")
