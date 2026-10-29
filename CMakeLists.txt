cmake_minimum_required(VERSION 3.16)

project(RaskKonsoll VERSION 1.0.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick CanvasPainter QuickControls2)

qt_standard_project_setup(REQUIRES 6.12)

# Flag padded/misaligned class layouts in Debug (see the alignment rule in
# AGENTS.md). Generator expression scopes it to Debug configs only.
add_compile_options($<$<CONFIG:Debug>:-Wpadded>)

set(MACOSX_BUNDLE_ICON_FILE konsoll.icns)
set(app_icon_macos "${CMAKE_CURRENT_SOURCE_DIR}/assets/konsoll.icns")
set_source_files_properties(${app_icon_macos} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

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
        TerminalView.qml
        controls/Menu.qml
        controls/MenuItem.qml
    SOURCES
        src/AboutDialog.h
        src/AboutDialog.mm
        src/TerminalBackend.cpp
        src/TerminalBackend.h
        src/pty/PtyProcess.cpp
        src/pty/PtyProcess.h
        src/term/Cell.h
        src/term/Screen.cpp
        src/term/Screen.h
        src/term/Terminal.cpp
        src/term/Terminal.h
        src/macs_translucent.mm
        src/macs_translucent.h
        src/WindowTranslucency.cpp
        src/WindowTranslucency.h
)

qt_add_resources(konsoll "/"
    PREFIX "/"
    FILES
        assets/konsoll.icns
        assets/konsoll.png
        assets/qtlogo-64.png
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(konsoll PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Konsoll"
    MACOSX_BUNDLE_GUI_IDENTIFIER com.github.rask-project
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# The generated QML type-registration TU includes the backend header as
# <TerminalBackend.h>, so src/ must be on the include path.
target_include_directories(konsoll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(konsoll
    PRIVATE Qt6::Quick Qt6::CanvasPainter Qt6::QuickControls2
)

# Enable the QML debug service so the QML Live Preview MCP tool
# (run_qmlPreview) can hot-reload QML edits without a rebuild.
# Scoped to Debug builds so the service is not shipped in Release.
target_compile_definitions(konsoll PRIVATE $<$<CONFIG:Debug>:QT_QML_DEBUG>)

install(TARGETS konsoll
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
