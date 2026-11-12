set(ICONS_SRC
    konsoll/index.theme
    konsoll/svg/add.svg
    konsoll/svg/appearance.svg
    konsoll/svg/arrow-prev.svg
    konsoll/svg/arrow-next.svg
    konsoll/svg/background-functions.svg
    konsoll/svg/background-running.svg
    konsoll/svg/background-saved.svg
    konsoll/svg/background-sessions.svg
    konsoll/svg/close.svg
    konsoll/svg/keyboard.svg
    konsoll/svg/kind-agent.svg
    konsoll/svg/kind-build.svg
    konsoll/svg/kind-container.svg
    konsoll/svg/kind-database.svg
    konsoll/svg/kind-editor.svg
    konsoll/svg/kind-git.svg
    konsoll/svg/kind-monitor.svg
    konsoll/svg/kind-multiplexer.svg
    konsoll/svg/kind-pager.svg
    konsoll/svg/kind-privileged.svg
    konsoll/svg/kind-remote.svg
    konsoll/svg/kind-repl.svg
    konsoll/svg/kind-shell.svg
    konsoll/svg/status-bell.svg
    konsoll/svg/status-failure.svg
    konsoll/svg/status-success.svg
    konsoll/svg/security.svg
    konsoll/svg/terminal.svg
    konsoll/svg/window.svg
)

qt_add_resources(konsoll "icons"
    PREFIX "/icons"
    FILES ${ICONS_SRC}
)
