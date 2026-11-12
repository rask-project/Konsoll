// Window listing and synthetic mouse input for scripted screenshots.
//
// Coordinates are global, top-left based points (the ones screencapture -R
// and CGWindowList use).
//
// Usage:
//   ctl windows PID                    on-screen windows of PID, front to back:
//                                      "ID X Y W H LAYER TITLE"
//   ctl click X Y [COUNT]              left click (COUNT = 2 double-clicks)
//   ctl rclick X Y                     right click
//   ctl move X Y                       move the pointer
//   ctl drag X1 Y1 X2 Y2 [HOLD_MS]     press, wait HOLD_MS (default 150),
//                                      move in 40 steps over ~1 s, release
import Cocoa
import CoreGraphics

let args = CommandLine.arguments

func usage() -> Never {
    FileHandle.standardError.write("usage: ctl windows PID | click X Y [N] | rclick X Y | move X Y | drag X1 Y1 X2 Y2 [HOLD_MS]\n".data(using: .utf8)!)
    exit(2)
}

func number(_ index: Int) -> Double {
    guard index < args.count, let value = Double(args[index]) else { usage() }
    return value
}

func point(_ index: Int) -> CGPoint { CGPoint(x: number(index), y: number(index + 1)) }

func post(_ type: CGEventType, _ at: CGPoint, _ button: CGMouseButton = .left, clicks: Int64 = 1) {
    let event = CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: at, mouseButton: button)!
    event.setIntegerValueField(.mouseEventClickState, value: clicks)
    event.post(tap: .cghidEventTap)
}

guard args.count >= 2 else { usage() }
switch args[1] {
case "windows":
    let pid = Int(number(2))
    let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
    for info in list where (info[kCGWindowOwnerPID as String] as? Int) == pid {
        let bounds = info[kCGWindowBounds as String] as? [String: Double] ?? [:]
        let fields: [Any] = [
            info[kCGWindowNumber as String] ?? 0,
            Int(bounds["X"] ?? 0), Int(bounds["Y"] ?? 0), Int(bounds["Width"] ?? 0), Int(bounds["Height"] ?? 0),
            info[kCGWindowLayer as String] ?? 0,
            info[kCGWindowName as String] ?? "",
        ]
        print(fields.map { "\($0)" }.joined(separator: " "))
    }
case "click":
    let at = point(2)
    let count = args.count > 4 ? Int64(number(4)) : 1
    post(.mouseMoved, at)
    usleep(80_000)
    for click in 1...max(count, 1) {
        post(.leftMouseDown, at, clicks: click)
        usleep(40_000)
        post(.leftMouseUp, at, clicks: click)
        usleep(60_000)
    }
case "rclick":
    let at = point(2)
    post(.mouseMoved, at)
    usleep(80_000)
    post(.rightMouseDown, at, .right)
    usleep(50_000)
    post(.rightMouseUp, at, .right)
case "move":
    post(.mouseMoved, point(2))
case "drag":
    let from = point(2), to = point(4)
    let hold = args.count > 6 ? UInt32(number(6) * 1000) : 150_000
    post(.mouseMoved, from)
    usleep(100_000)
    post(.leftMouseDown, from)
    usleep(hold)
    let steps = 40
    for step in 1...steps {
        let t = Double(step) / Double(steps)
        post(.leftMouseDragged, CGPoint(x: from.x + (to.x - from.x) * t, y: from.y + (to.y - from.y) * t))
        usleep(25_000)
    }
    usleep(300_000)
    post(.leftMouseUp, to)
default:
    usage()
}
