cmake_minimum_required(VERSION 3.21.1)

set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture")
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.4" CACHE STRING "Minimum supported macOS version")

project(RaskKonsoll VERSION 1.6.1 LANGUAGES CXX OBJCXX)

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

if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/dist" CACHE PATH
        "Directory for the installed macOS application bundle" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Quick CanvasPainter QuickControls2 QuickDialogs2 Svg)

qt_standard_project_setup(REQUIRES 6.12)

# Flag padded/misaligned class layouts in Debug (see the alignment rule in
# AGENTS.md). Generator expression scopes it to Debug configs only.
add_compile_options($<$<CONFIG:Debug>:-Wpadded>)

set(MACOSX_BUNDLE_ICON_FILE konsoll.icns)
set(app_icon_macos "${CMAKE_CURRENT_SOURCE_DIR}/assets/konsoll.icns")
set_source_files_properties(${app_icon_macos} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

set(KONSOLL_CODESIGN_IDENTITY "" CACHE STRING
    "Developer ID Application identity used to sign the macOS app bundle")

qt_add_executable(konsoll
    main.cpp
    ${app_icon_macos}
)

qt_add_qml_module(konsoll
    URI konsoll
    VERSION 1.0
    NAMESPACE konsoll
    QML_FILES
        Main.qml
        SaveTextDialog.qml
        SettingsDialog.qml
        TerminalWorkspace.qml
        TerminalPaneLayout.qml
        TerminalView.qml
        controls/TerminalTabStrip.qml
        controls/TerminalTabButton.qml
        controls/ToolButton.qml
        controls/Menu.qml
        controls/MenuItem.qml
        controls/SearchBar.qml
    SOURCES
        src/AboutDialog.h
        src/AboutDialog.mm
        src/TerminalBackend.cpp
        src/TerminalBackend.h
        src/TerminalManager.cpp
        src/TerminalManager.h
        src/pty/PtyProcess.cpp
        src/pty/PtyProcess.h
        src/term/Cell.h
        src/term/Screen.cpp
        src/term/Screen.h
        src/term/Terminal.cpp
        src/term/Terminal.h
        src/MacOSTranslucency.mm
        src/MacOSTranslucency.h
        src/WindowTranslucency.cpp
        src/WindowTranslucency.h
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
)

add_subdirectory(resources/icons)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(konsoll PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Konsoll"
    MACOSX_BUNDLE_GUI_IDENTIFIER com.github.rask-project.konsoll
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# The generated QML type-registration TU includes the backend header as
# <TerminalBackend.h>, so src/ must be on the include path.
target_include_directories(konsoll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(konsoll PRIVATE
    KONSOLL_VERSION="${PROJECT_VERSION}"
    KONSOLL_GIT_COMMIT="${KONSOLL_GIT_COMMIT}"
)

target_link_libraries(konsoll
    PRIVATE Qt6::Quick Qt6::CanvasPainter Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::Svg
)

install(TARGETS konsoll
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(APPLE)
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

    add_custom_target(package-macos
        COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG>
        DEPENDS konsoll
        COMMENT "Deploying self-contained Konsoll bundle to ${CMAKE_INSTALL_PREFIX}"
        VERBATIM
    )
endif()
