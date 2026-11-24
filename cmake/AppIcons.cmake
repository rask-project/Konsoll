# App icons. macOS 26 and later draw an app's icon from an Icon Composer
# document (assets/*.icon) compiled into Assets.car (CFBundleIconName); a
# plain .icns whose shape is not the system's rounded square is shrunk onto
# a gray tile. actool compiles each .icon into an Assets.car plus a .icns
# that older macOS versions read (CFBundleIconFile).
#
# Without actool (Command Line Tools only) the bundles keep the legacy
# assets/*.icns.
#
# Sets KONSOLL_APP_ICON_FILES (the main bundle's icon resources, sources of
# the konsoll target), MACOSX_BUNDLE_ICON_FILE, KONSOLL_EDITOR_ICON_FILES
# (copied into the editor's helper bundle), KONSOLL_EDITOR_ICON_FILE, and
# KONSOLL_ICON_NAME_KEY / KONSOLL_EDITOR_ICON_NAME_KEY (the CFBundleIconName
# plist entries, empty without actool).

execute_process(
    COMMAND xcrun --find actool
    OUTPUT_VARIABLE konsoll_actool
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE konsoll_actool_result)

# The Debug kit may leave the deployment target empty; actool needs one.
set(konsoll_icon_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(NOT konsoll_icon_target)
    set(konsoll_icon_target 14.4)
endif()

# Compiles assets/NAME.icon into ${CMAKE_BINARY_DIR}/icons/NAME and sets
# OUT_VAR to its Assets.car and NAME.icns.
function(konsoll_compile_icon name out_var)
    set(icon "${CMAKE_CURRENT_SOURCE_DIR}/assets/${name}.icon")
    set(out_dir "${CMAKE_BINARY_DIR}/icons/${name}")
    file(GLOB icon_inputs CONFIGURE_DEPENDS "${icon}/icon.json" "${icon}/Assets/*")
    set(outputs "${out_dir}/Assets.car" "${out_dir}/${name}.icns")
    add_custom_command(
        OUTPUT ${outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND xcrun actool "${icon}" --compile "${out_dir}"
            --platform macosx --target-device mac
            --minimum-deployment-target ${konsoll_icon_target}
            --app-icon ${name} --include-all-app-icons
            --output-partial-info-plist "${out_dir}/partial.plist"
            --errors --warnings
        DEPENDS ${icon_inputs}
        COMMENT "Compiling ${name}.icon"
        VERBATIM)
    set(${out_var} ${outputs} PARENT_SCOPE)
endfunction()

if(konsoll_actool_result EQUAL 0 AND konsoll_actool)
    konsoll_compile_icon(konsoll KONSOLL_APP_ICON_FILES)
    konsoll_compile_icon(konsoll-nvim KONSOLL_EDITOR_ICON_FILES)
    set(KONSOLL_ICON_NAME_KEY "<key>CFBundleIconName</key>\n\t<string>konsoll</string>")
    set(KONSOLL_EDITOR_ICON_NAME_KEY "<key>CFBundleIconName</key>\n\t<string>konsoll-nvim</string>")
else()
    message(STATUS "actool not found (Xcode): using the legacy .icns app icons")
    set(KONSOLL_APP_ICON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/konsoll.icns")
    set(KONSOLL_EDITOR_ICON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/konsoll-nvim.icns")
    set(KONSOLL_ICON_NAME_KEY "")
    set(KONSOLL_EDITOR_ICON_NAME_KEY "")
endif()
set(MACOSX_BUNDLE_ICON_FILE konsoll.icns)
set(KONSOLL_EDITOR_ICON_FILE konsoll-nvim.icns)
set_source_files_properties(${KONSOLL_APP_ICON_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
# Built before the target, so the editor's POST_BUILD step can copy them.
add_custom_target(konsoll-icons DEPENDS ${KONSOLL_APP_ICON_FILES} ${KONSOLL_EDITOR_ICON_FILES})
