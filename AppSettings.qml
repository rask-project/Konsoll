import QtCore

Settings {
    category: "General"

    property real fontPointSize: 15
    property string fontFamily: "Menlo"
    property int fontWeight: 400
    property bool fontItalic: false
    property real windowAlpha: 0.85
    property bool blurEnabled: true
    // Cursor until a program picks one: 0 block, 1 underline, 2 bar, 3 hollow block.
    property int cursorShape: 0
    property bool cursorBlink: false

    // Settings > Security: what applications in the terminal may do.
    property int clipboardRead: 1 // OSC 52 read: 0 never, 1 ask, 2 always
    property bool allowTitleReport: false // XTWINOPS 20/21
    property bool allowWindowOps: false   // XTWINOPS move/resize/iconify
    property bool allowNotifications: true // OSC 9 / OSC 777 desktop notifications

    // Settings > Keyboard.
    property bool optionAsMeta: false // Option sends ESC + key instead of composing
}
