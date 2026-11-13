cmake_minimum_required(VERSION 3.21.1)

set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture")
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.4" CACHE STRING "Minimum supported macOS version")

project(RaskKonsoll VERSION 3.7.0 LANGUAGES CXX OBJCXX)

execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE KONSOLL_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT KONSOLL_GIT_COMMIT)
    set(KONSOLL_GIT_COMMIT "unknown")
endif()

execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE KONSOLL_DISPLAY_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(KONSOLL_DISPLAY_VERSION)
    string(REGEX REPLACE "^v" "" KONSOLL_DISPLAY_VERSION "${KONSOLL_DISPLAY_VERSION}")
else()
    set(KONSOLL_DISPLAY_VERSION ${PROJECT_VERSION})
endif()

# CFBundleVersion, which Sparkle compares to find a newer release: the tag's
# version plus a fourth number that orders its pre-releases before it
# (3.7.0-alpha.2 -> 3.7.0.2, -beta.2 -> 3.7.0.102, -rc.2 -> 3.7.0.502,
# 3.7.0 -> 3.7.0.1000).
set(KONSOLL_BUNDLE_VERSION "${PROJECT_VERSION}.1000")
if(KONSOLL_DISPLAY_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)(-(alpha|beta|rc)\\.([0-9]+))?$")
    set(release_base "${CMAKE_MATCH_1}")
    set(prerelease_kind "${CMAKE_MATCH_3}")
    set(prerelease_number "${CMAKE_MATCH_4}")
    if(prerelease_kind STREQUAL "alpha")
        set(release_build "${prerelease_number}")
    elseif(prerelease_kind STREQUAL "beta")
        math(EXPR release_build "100 + ${prerelease_number}")
    elseif(prerelease_kind STREQUAL "rc")
        math(EXPR release_build "500 + ${prerelease_number}")
    else()
        set(release_build 1000)
    endif()
    set(KONSOLL_BUNDLE_VERSION "${release_base}.${release_build}")
endif()

if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/dist" CACHE PATH
        "Directory for the installed macOS application bundle" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Quick QuickControls2 QuickDialogs2 Svg Network)

qt_standard_project_setup(REQUIRES 6.12)

# Flag padded/misaligned class layouts in Debug (see the alignment rule in
# AGENTS.md). Generator expression scopes it to Debug configs only.
add_compile_options($<$<CONFIG:Debug>:-Wpadded>)

set(MACOSX_BUNDLE_ICON_FILE konsoll.icns)
set(app_icon_macos "${CMAKE_CURRENT_SOURCE_DIR}/assets/konsoll.icns")
set_source_files_properties(${app_icon_macos} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

set(KONSOLL_CODESIGN_IDENTITY "" CACHE STRING
    "Developer ID Application identity used to sign the macOS app bundle")

# Updates (Sparkle). The appcast lists every release; the release workflow
# adds each tag to it (tools/update-appcast.py). Debug builds do not update
# themselves: without a feed URL the updater stays off.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(default_feed_url "")
else()
    set(default_feed_url "https://raw.githubusercontent.com/rask-project/Konsoll/appcast/appcast.xml")
endif()
set(KONSOLL_UPDATE_FEED_URL "${default_feed_url}" CACHE STRING
    "Sparkle appcast URL (empty turns the updater off)")
# The EdDSA public key matching the private key the release workflow signs
# updates with (generate_keys, SPARKLE_ED_PRIVATE_KEY secret).
set(KONSOLL_UPDATE_PUBLIC_KEY "hb9+LAm6J+NKJX6j3LRQL/G4iV4h1/eJOwS/lYJPDto=" CACHE STRING
    "Sparkle EdDSA public key (empty turns the updater off)")

include(cmake/Sparkle.cmake)

set(KONSOLL_SPARKLE_PLIST_KEYS "")
if(KONSOLL_UPDATE_FEED_URL AND KONSOLL_UPDATE_PUBLIC_KEY)
    string(CONCAT KONSOLL_SPARKLE_PLIST_KEYS
        "\t<key>SUFeedURL</key>\n"
        "\t<string>${KONSOLL_UPDATE_FEED_URL}</string>\n"
        "\t<key>SUPublicEDKey</key>\n"
        "\t<string>${KONSOLL_UPDATE_PUBLIC_KEY}</string>\n"
        "\t<key>SUVerifyUpdateBeforeExtraction</key>\n"
        "\t<true/>\n"
        # Updates are on by default, without Sparkle's first-run question:
        # checked daily, downloaded in the background and installed on quit.
        # Settings > Updates turns them off.
        "\t<key>SUEnableAutomaticChecks</key>\n"
        "\t<true/>\n"
        "\t<key>SUAutomaticallyUpdate</key>\n"
        "\t<true/>\n"
    )
endif()

qt_add_executable(konsoll
    main.cpp
    ${app_icon_macos}
)

qt_add_qml_module(konsoll
    URI konsoll
    VERSION 1.0
    NAMESPACE konsoll
    QML_FILES
        AppSettings.qml
        Main.qml
        TerminalWindow.qml
        SaveTextDialog.qml
        SettingsWindow.qml
        BackgroundWindow.qml
        TerminalWorkspace.qml
        TerminalPaneLayout.qml
        TerminalView.qml
        controls/TerminalTabStrip.qml
        controls/TerminalTabButton.qml
        controls/TabActivityIndicator.qml
        controls/GlassSurface.qml
        controls/ToolButton.qml
        controls/AgentConnectBanner.qml
        controls/AgentReadRequestBanner.qml
        controls/SettingsSidebarItem.qml
        controls/FormGroup.qml
        controls/FormRow.qml
        controls/Menu.qml
        controls/MenuItem.qml
        controls/SearchBar.qml
    SOURCES
        src/AboutDialog.h
        src/AboutDialog.mm
        src/AgentCommands.cpp
        src/AgentCommands.h
        src/AgentIntegrations.cpp
        src/AgentIntegrations.h
        src/AgentStatus.cpp
        src/AgentStatus.h
        src/AppQuitGuard.h
        src/AppQuitGuard.mm
        src/BackgroundJobs.cpp
        src/BackgroundJobs.h
        src/KeyEncoder.cpp
        src/KeyEncoder.h
        src/McpServer.cpp
        src/McpServer.h
        src/NativeDialogs.h
        src/NativeKeyTracker.cpp
        src/NativeKeyTracker.h
        src/NativeDialogs.mm
        src/ProcessKind.cpp
        src/ProcessKind.h
        src/ShellFunctions.cpp
        src/ShellFunctions.h
        src/ShellStartup.cpp
        src/ShellStartup.h
        src/SshIntegration.cpp
        src/SshIntegration.h
        src/ScrollbackSettings.cpp
        src/ScrollbackSettings.h
        src/Notifications.h
        src/Notifications.mm
        src/TerminalBackend.cpp
        src/TerminalBackend.h
        src/TerminalManager.cpp
        src/TerminalManager.h
        src/TerminalRenderer.cpp
        src/TerminalRenderer.h
        src/Updater.h
        src/Updater.mm
        src/pty/PtyProcess.cpp
        src/pty/PtyProcess.h
        src/pty/PtyWorker.cpp
        src/pty/PtyWorker.h
        src/parserselftest.cpp
        src/renderdump.cpp
        src/renderdump.h
        src/screendump.cpp
        src/screendump.h
        src/selftest.cpp
        src/selftest.h
        src/term/Cell.h
        src/term/Charsets.cpp
        src/term/Charsets.h
        src/term/CharWidth.cpp
        src/term/CharWidth.h
        src/term/Grapheme.cpp
        src/term/Grapheme.h
        src/term/Graphics.cpp
        src/term/Graphics.h
        src/term/Palette.cpp
        src/term/Palette.h
        src/term/Screen.cpp
        src/term/Screen.h
        src/term/Scrollback.cpp
        src/term/Scrollback.h
        src/term/ScreenRect.cpp
        src/term/Terminal.cpp
        src/term/Terminal.h
        src/term/TerminalReports.cpp
        src/GlassEffect.cpp
        src/GlassEffect.h
        src/MacOSTranslucency.mm
        src/MacOSTranslucency.h
        src/WindowTranslucency.cpp
        src/WindowTranslucency.h
        src/WindowScreen.cpp
        src/WindowScreen.h
        src/WindowRegistry.cpp
        src/WindowRegistry.h
        src/WindowState.cpp
        src/WindowState.h
        src/WindowStateStore.cpp
        src/WindowStateStore.h
)

qt_add_resources(konsoll "/"
    PREFIX "/"
    FILES
        assets/konsoll.icns
        assets/konsoll.png
        assets/qtlogo-64.png

        shell-integration/konsoll.zsh
        shell-integration/konsoll.bash
        shell-integration/konsoll.fish
        shell-integration/konsoll-agent-hook
        shell-integration/konsoll-ssh.sh
        shell-integration/konsoll-remote.sh
        shell-integration/konsoll-remote-hook
        integrations/opencode/konsoll.js
)

add_subdirectory(resources/icons)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
# Debug builds get a bundle id of their own: with the release installed too,
# macOS would otherwise resolve the shared id to the other copy (a click on a
# notification launches it) and share its notification permission.
# QSettings is keyed by the organization domain (main.cpp), not this id.
set(KONSOLL_BUNDLE_ID com.github.rask-project.konsoll)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    string(APPEND KONSOLL_BUNDLE_ID ".debug")
endif()

set_target_properties(konsoll PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Konsoll"
    MACOSX_BUNDLE_GUI_IDENTIFIER ${KONSOLL_BUNDLE_ID}
    MACOSX_BUNDLE_BUNDLE_VERSION ${KONSOLL_BUNDLE_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${KONSOLL_DISPLAY_VERSION}
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# The generated QML type-registration TU includes the backend header as
# <TerminalBackend.h>, so src/ must be on the include path.
target_include_directories(konsoll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(konsoll PRIVATE
    KONSOLL_VERSION="${KONSOLL_DISPLAY_VERSION}"
    KONSOLL_GIT_COMMIT="${KONSOLL_GIT_COMMIT}"
    KONSOLL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(konsoll
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::Svg Qt6::Network
    "-framework QuartzCore"
    "-framework UserNotifications"
    "-framework Sparkle"
    z
)

# Sparkle.framework is embedded in the bundle's Frameworks directory, which
# Qt already puts on the executable's rpath.
get_filename_component(sparkle_framework_dir "${SPARKLE_FRAMEWORK}" DIRECTORY)
# -iframework: a system framework path, so its headers raise no warnings.
target_compile_options(konsoll PRIVATE "-iframework${sparkle_framework_dir}")
target_link_options(konsoll PRIVATE "-F${sparkle_framework_dir}")

install(TARGETS konsoll
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(APPLE)
    # The linker signs the executable alone, leaving the bundle's Info.plist
    # unbound: macOS then refuses notifications (UNErrorDomain 1, no prompt).
    # Sign the build tree's bundle ad hoc; the install step signs it again.
    # Sparkle.framework goes in first (ditto keeps its symlinks), so the
    # signature covers it.
    add_custom_command(TARGET konsoll POST_BUILD
        COMMAND rm -rf "$<TARGET_BUNDLE_DIR:konsoll>/Contents/Frameworks/Sparkle.framework"
        COMMAND ditto "${SPARKLE_FRAMEWORK}"
            "$<TARGET_BUNDLE_DIR:konsoll>/Contents/Frameworks/Sparkle.framework"
        COMMENT "Embedding Sparkle.framework"
        VERBATIM
    )
    add_custom_command(TARGET konsoll POST_BUILD
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:konsoll>"
        COMMENT "Signing konsoll.app ad hoc (binds Info.plist for notifications)"
        VERBATIM
    )

    set(deploy_tool_options_arg)
    if(KONSOLL_CODESIGN_IDENTITY)
        qt_policy(SET QTP0007 NEW)
        list(APPEND deploy_tool_options_arg
            "-sign-for-notarization=${KONSOLL_CODESIGN_IDENTITY}"
        )
    endif()

    qt_generate_deploy_qml_app_script(
        TARGET konsoll
        OUTPUT_SCRIPT deploy_script
        DEPLOY_TOOL_OPTIONS ${deploy_tool_options_arg}
    )
    install(SCRIPT ${deploy_script})
    install(CODE "
        set(APP_BUNDLE \"\${CMAKE_INSTALL_PREFIX}/konsoll.app\")
        set(CODESIGN_IDENTITY \"${KONSOLL_CODESIGN_IDENTITY}\")
        include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/PruneBundle.cmake\")
    ")

    add_custom_target(package-macos
        COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG>
        DEPENDS konsoll
        COMMENT "Deploying self-contained Konsoll bundle to ${CMAKE_INSTALL_PREFIX}"
        VERBATIM
    )
endif()
