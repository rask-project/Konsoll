// A floating caption for GIFs: shows the text of a file in a dark pill.
//
// The recording script writes the key it is about to press (or a caption)
// to the file; an empty file hides the pill. The pill never takes clicks.
//
// Usage: hud FILE X Y    X, Y = bottom-center of the pill, in global
//                        top-left coordinates (points)
import Cocoa

let args = CommandLine.arguments
guard args.count == 4, let centerX = Double(args[2]), let bottomY = Double(args[3]) else {
    FileHandle.standardError.write("usage: hud FILE X Y\n".data(using: .utf8)!)
    exit(2)
}
let path = args[1]

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let primary = NSScreen.screens[0].frame

let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 10, height: 10), styleMask: .borderless,
                      backing: .buffered, defer: false)
window.isOpaque = false
window.backgroundColor = .clear
window.level = .floating
window.ignoresMouseEvents = true
window.hasShadow = true

let box = NSView()
box.wantsLayer = true
box.layer!.backgroundColor = NSColor(white: 0.08, alpha: 0.88).cgColor
box.layer!.cornerRadius = 14
box.layer!.borderColor = NSColor(white: 1, alpha: 0.18).cgColor
box.layer!.borderWidth = 1

let label = NSTextField(labelWithString: "")
label.font = NSFont.systemFont(ofSize: 26, weight: .semibold)
label.textColor = .white
box.addSubview(label)
window.contentView = box

var shown = "\u{0}"
Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
    let text = ((try? String(contentsOfFile: path, encoding: .utf8)) ?? "")
        .trimmingCharacters(in: .whitespacesAndNewlines)
    if text == shown { return }
    shown = text
    if text.isEmpty {
        window.orderOut(nil)
        return
    }
    label.stringValue = text
    label.sizeToFit()
    let size = NSSize(width: label.frame.width + 36, height: label.frame.height + 18)
    label.setFrameOrigin(NSPoint(x: 18, y: 9))
    // AppKit's origin is bottom-left; convert from the top-left Y we were given.
    window.setFrame(NSRect(x: centerX - size.width / 2, y: primary.height - bottomY,
                           width: size.width, height: size.height), display: true)
    window.orderFrontRegardless()
}
app.run()
