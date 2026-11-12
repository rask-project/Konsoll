// A full-screen, click-through background for the built-in display: a dark
// gradient, or an image (the desktop wallpaper, from `docshot.sh wallpaper`).
//
// Konsoll's window is see-through, so screenshots show whatever is behind it.
// Run this behind the app so every shot gets the same background and no
// other window shows through.
//
// Usage: backdrop [IMAGE]        (runs until killed)
import Cocoa

final class GradientView: NSView {
    override func draw(_ dirtyRect: NSRect) {
        NSGradient(starting: NSColor(srgbRed: 0.20, green: 0.22, blue: 0.30, alpha: 1),
                   ending: NSColor(srgbRed: 0.09, green: 0.10, blue: 0.14, alpha: 1))!
            .draw(in: bounds, angle: -60)
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

// screens[0] is the display with the menu bar, whose top-left is (0, 0) in the
// global coordinates screencapture -R and CGWindowList use.
let window = NSWindow(contentRect: NSScreen.screens[0].frame, styleMask: .borderless,
                      backing: .buffered, defer: false)
let arguments = CommandLine.arguments
if arguments.count > 1, let image = NSImage(contentsOfFile: arguments[1]) {
    let view = NSImageView()
    view.image = image
    view.imageScaling = .scaleAxesIndependently
    window.contentView = view
} else {
    window.contentView = GradientView()
}
window.level = .normal
window.isOpaque = true
window.ignoresMouseEvents = true
window.orderFrontRegardless()
app.run()
