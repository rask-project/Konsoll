// Draws the Neovim (Konsoll) icon: Konsoll's icon with its ">_" prompt
// replaced by the Neovim "N" (assets/neovim-mark.svg), then the .icns the
// editor bundle uses (CMakeLists.txt).
//
//   swift tools/make-editor-icon.swift [CENTER_X CENTER_Y HEIGHT]
//
// CENTER_X, CENTER_Y and HEIGHT place the N, in pixels of the 1254 px icon
// (top-down); by default where the prompt was: its left edge on the ">"'s
// (x 251), centered on the prompt's line.
import AppKit

let arguments = CommandLine.arguments
let custom = arguments.count > 3
let centerX = custom ? Double(arguments[1])! : 354.5
let centerY = custom ? Double(arguments[2])! : 445
let height = custom ? Double(arguments[3])! : 250
// The prompt's pixels (x 251-594, y 338-552) with a margin, top-down.
let promptLeft = 236, promptRight = 610, promptTop = 323, promptBottom = 568
let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
let assets = root.appendingPathComponent("assets")

// Paints the screen's background over the prompt: each pixel blends the
// pixels on the area's edges (left/right and top/bottom), which keeps the
// screen's slight vertical gradient.
func erasePrompt(_ bitmap: NSBitmapImageRep) {
    func pixel(_ x: Int, _ y: Int) -> [Int] {
        var value = [Int](repeating: 0, count: 4)
        bitmap.getPixel(&value, atX: x, y: y)
        return value
    }
    let left = (promptTop...promptBottom).map { pixel(promptLeft, $0) }
    let right = (promptTop...promptBottom).map { pixel(promptRight, $0) }
    let top = (promptLeft...promptRight).map { pixel($0, promptTop) }
    let bottom = (promptLeft...promptRight).map { pixel($0, promptBottom) }
    for y in promptTop + 1..<promptBottom {
        for x in promptLeft + 1..<promptRight {
            let tx = Double(x - promptLeft) / Double(promptRight - promptLeft)
            let ty = Double(y - promptTop) / Double(promptBottom - promptTop)
            let row = y - promptTop, column = x - promptLeft
            var value = (0..<4).map { channel -> Int in
                let across = Double(left[row][channel]) * (1 - tx) + Double(right[row][channel]) * tx
                let down = Double(top[column][channel]) * (1 - ty) + Double(bottom[column][channel]) * ty
                return Int(((across + down) / 2).rounded())
            }
            bitmap.setPixel(&value, atX: x, y: y)
        }
    }
}

let base = NSBitmapImageRep(data: try! Data(contentsOf: assets.appendingPathComponent("konsoll.png")))!
let mark = NSImage(contentsOf: assets.appendingPathComponent("neovim-mark.svg"))!
let size = NSSize(width: base.pixelsWide, height: base.pixelsHigh)

let canvas = NSBitmapImageRep(
    bitmapDataPlanes: nil, pixelsWide: base.pixelsWide, pixelsHigh: base.pixelsHigh,
    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
canvas.size = size
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: canvas)
NSGraphicsContext.current!.imageInterpolation = .high
base.draw(in: NSRect(origin: .zero, size: size))
NSGraphicsContext.restoreGraphicsState()
erasePrompt(canvas)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: canvas)
NSGraphicsContext.current!.imageInterpolation = .high
let width = height * mark.size.width / mark.size.height
let rect = NSRect(x: centerX - width / 2, y: size.height - centerY - height / 2, width: width, height: height)
mark.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1)
NSGraphicsContext.restoreGraphicsState()

let png = assets.appendingPathComponent("konsoll-nvim.png")
try! canvas.representation(using: .png, properties: [:])!.write(to: png)

// The iconset iconutil turns into the .icns: 16-512 pt at 1x and 2x.
let iconset = FileManager.default.temporaryDirectory.appendingPathComponent("konsoll-nvim.iconset")
try? FileManager.default.removeItem(at: iconset)
try! FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
let image = NSImage(size: size)
image.addRepresentation(canvas)
for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = points * scale
        let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        NSGraphicsContext.current!.imageInterpolation = .high
        image.draw(in: NSRect(x: 0, y: 0, width: pixels, height: pixels))
        NSGraphicsContext.restoreGraphicsState()
        let name = scale == 1 ? "icon_\(points)x\(points).png" : "icon_\(points)x\(points)@2x.png"
        try! rep.representation(using: .png, properties: [:])!.write(to: iconset.appendingPathComponent(name))
    }
}
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["--convert", "icns", "--output", assets.appendingPathComponent("konsoll-nvim.icns").path, iconset.path]
try! iconutil.run()
iconutil.waitUntilExit()
print(iconutil.terminationStatus == 0 ? "wrote assets/konsoll-nvim.png and .icns" : "iconutil failed")
