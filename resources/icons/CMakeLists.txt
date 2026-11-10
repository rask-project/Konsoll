set(ICONS_SRC
    konsoll/index.theme
    konsoll/svg/add.svg
    konsoll/svg/appearance.svg
    konsoll/svg/arrow-prev.svg
    konsoll/svg/arrow-next.svg
    konsoll/svg/close.svg
    konsoll/svg/window.svg
)

qt_add_resources(konsoll "icons"
    PREFIX "/icons"
    FILES ${ICONS_SRC}
)
