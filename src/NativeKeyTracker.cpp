#include "NativeKeyTracker.h"

#include <QKeyEvent>

#include <array>
#include <atomic>

namespace konsoll
{

namespace
{
std::atomic<int> &lastKey()
{
    static std::atomic<int> value{-1};
    return value;
}

// macOS kVK_ANSI_* virtual key codes 0x00-0x32 -> the US-layout character.
constexpr std::array<char, 0x33> kUsLayout{
    'a', 's', 'd', 'f', 'h', 'g', 'z', 'x', 'c', 'v', 0,   'b', 'q', 'w', 'e', 'r', 'y',
    't', '1', '2', '3', '4', '6', '5', '=', '9', '7', '-', '8', '0', ']', 'o', 'u', '[',
    'i', 'p', 0,   'l', 'j', '\'', 'k', ';', '\\', ',', '/', 'n', 'm', '.', 0,   0,   '`',
};
} // namespace

int NativeKeyTracker::lastVirtualKey()
{
    return lastKey().load(std::memory_order_relaxed);
}

bool NativeKeyTracker::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        lastKey().store(static_cast<int>(static_cast<QKeyEvent *>(event)->nativeVirtualKey()), std::memory_order_relaxed);
    }
    return QObject::eventFilter(watched, event);
}

int usLayoutKey(int virtualKey)
{
    if (virtualKey < 0 || virtualKey >= static_cast<int>(kUsLayout.size())) {
        return 0;
    }
    return kUsLayout[static_cast<std::size_t>(virtualKey)];
}

} // namespace konsoll
