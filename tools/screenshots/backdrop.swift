// A full-screen, click-through dark gradient for the built-in display.
//
// Konsoll's window is see-through, so screenshots show whatever is behind it.
// Run this behind the app so every shot gets the same neutral background.
//
// Usage: backdrop        (runs until killed)
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
window.contentView = GradientView()
window.level = .normal
window.isOpaque = true
window.ignoresMouseEvents = true
window.orderFrontRegardless()
app.run()
