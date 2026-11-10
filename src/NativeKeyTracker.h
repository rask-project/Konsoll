#pragma once

#include <QObject>

namespace konsoll
{

// Application-wide event filter that remembers the native key code
// (macOS virtual key) of the last key event, which QML's KeyEvent does not
// expose. The kitty keyboard protocol reports the US-layout key at that
// physical position (its "base layout key").
class NativeKeyTracker : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // The virtual key of the key event being delivered, or -1.
    static int lastVirtualKey();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

// The US-layout character (lowercase) at macOS virtual key `virtualKey`, or
// 0 when it is not a character key.
int usLayoutKey(int virtualKey);

} // namespace konsoll
