set(ICONS_SRC
    konsoll/index.theme
    konsoll/svg/arrow-prev.svg
    konsoll/svg/arrow-next.svg
    konsoll/svg/close.svg
)

qt_add_resources(konsoll "icons"
    PREFIX "/icons"
    FILES ${ICONS_SRC}
)
