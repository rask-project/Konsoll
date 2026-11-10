#include "KeyEncoder.h"

#include <Qt>

#include <algorithm>
#include <array>

namespace konsoll
{

namespace
{

constexpr int kNoNumber = 0;

// How a cursor/function key is sent.
constexpr int kCursorKey = 0;   // CSI X, or SS3 X in application cursor mode
constexpr int kSs3Key = 1;      // SS3 X unmodified (F1-F4)
constexpr int kTildeKey = 2;    // CSI n ~

struct SpecialKey {
    int key;
    int number; // the CSI n ~ number, or kNoNumber
    int final;  // final byte
    int kind;
};

constexpr std::array kSpecialKeys{
    SpecialKey{.key = Qt::Key_Up, .number = kNoNumber, .final = 'A', .kind = kCursorKey},
    SpecialKey{.key = Qt::Key_Down, .number = kNoNumber, .final = 'B', .kind = kCursorKey},
    SpecialKey{.key = Qt::Key_Right, .number = kNoNumber, .final = 'C', .kind = kCursorKey},
    SpecialKey{.key = Qt::Key_Left, .number = kNoNumber, .final = 'D', .kind = kCursorKey},
    SpecialKey{.key = Qt::Key_Home, .number = kNoNumber, .final = 'H', .kind = kCursorKey},
    SpecialKey{.key = Qt::Key_End, .number = kNoNumber, .final = 'F', .kind = kCursorKey},
    SpecialKey{.key = Qt::Key_F1, .number = kNoNumber, .final = 'P', .kind = kSs3Key},
    SpecialKey{.key = Qt::Key_F2, .number = kNoNumber, .final = 'Q', .kind = kSs3Key},
    SpecialKey{.key = Qt::Key_F3, .number = kNoNumber, .final = 'R', .kind = kSs3Key},
    SpecialKey{.key = Qt::Key_F4, .number = kNoNumber, .final = 'S', .kind = kSs3Key},
    SpecialKey{.key = Qt::Key_Insert, .number = 2, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_Delete, .number = 3, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_PageUp, .number = 5, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_PageDown, .number = 6, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F5, .number = 15, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F6, .number = 17, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F7, .number = 18, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F8, .number = 19, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F9, .number = 20, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F10, .number = 21, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F11, .number = 23, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F12, .number = 24, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F13, .number = 25, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F14, .number = 26, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F15, .number = 28, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F16, .number = 29, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F17, .number = 31, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F18, .number = 32, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F19, .number = 33, .final = '~', .kind = kTildeKey},
    SpecialKey{.key = Qt::Key_F20, .number = 34, .final = '~', .kind = kTildeKey},
};

// DECKPAM: keypad keys send SS3 sequences.
struct KeypadKey {
    int key;
    int final;
};

constexpr std::array kKeypadKeys{
    KeypadKey{.key = Qt::Key_0, .final = 'p'},        KeypadKey{.key = Qt::Key_1, .final = 'q'},
    KeypadKey{.key = Qt::Key_2, .final = 'r'},        KeypadKey{.key = Qt::Key_3, .final = 's'},
    KeypadKey{.key = Qt::Key_4, .final = 't'},        KeypadKey{.key = Qt::Key_5, .final = 'u'},
    KeypadKey{.key = Qt::Key_6, .final = 'v'},        KeypadKey{.key = Qt::Key_7, .final = 'w'},
    KeypadKey{.key = Qt::Key_8, .final = 'x'},        KeypadKey{.key = Qt::Key_9, .final = 'y'},
    KeypadKey{.key = Qt::Key_Period, .final = 'n'},   KeypadKey{.key = Qt::Key_Comma, .final = 'l'},
    KeypadKey{.key = Qt::Key_Enter, .final = 'M'},    KeypadKey{.key = Qt::Key_Plus, .final = 'k'},
    KeypadKey{.key = Qt::Key_Minus, .final = 'm'},    KeypadKey{.key = Qt::Key_Asterisk, .final = 'j'},
    KeypadKey{.key = Qt::Key_Slash, .final = 'o'},    KeypadKey{.key = Qt::Key_Equal, .final = 'X'},
};

QByteArray modifierParam(unsigned int mods)
{
    return QByteArray::number(static_cast<int>(mods) + 1);
}

QByteArray encodeSpecial(const SpecialKey &special, unsigned int mods, unsigned int modes)
{
    const char final = static_cast<char>(special.final);
    if ((modes & KeyMode::Vt52) != 0 && special.kind != kTildeKey) {
        return QByteArray("\x1b") + final; // VT52: ESC A ... ESC D, ESC P ... ESC S
    }
    if (special.kind == kTildeKey) {
        QByteArray out = "\x1b[" + QByteArray::number(special.number);
        if (mods != 0) {
            out += ";" + modifierParam(mods);
        }
        return out + final;
    }
    if (mods != 0) {
        return "\x1b[1;" + modifierParam(mods) + final;
    }
    const bool ss3 = special.kind == kSs3Key || (modes & KeyMode::ApplicationCursor) != 0;
    return QByteArray(ss3 ? "\x1bO" : "\x1b[") + final;
}

// Control byte for Ctrl+<key>: letters, and the xterm punctuation/digit
// aliases (Ctrl+Space / Ctrl+2 = NUL, Ctrl+[ = ESC, ... Ctrl+/ = US).
QByteArray controlByte(int key, const QString &text)
{
    if (!text.isEmpty() && text.at(0).unicode() < 0x20) {
        return text.toLatin1(); // the system already produced the control byte
    }
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return QByteArray(1, static_cast<char>(key - Qt::Key_A + 1));
    }
    struct Alias {
        int key;
        int byte;
    };
    static constexpr std::array kAliases{
        Alias{.key = Qt::Key_Space, .byte = 0x00},        Alias{.key = Qt::Key_At, .byte = 0x00},
        Alias{.key = Qt::Key_2, .byte = 0x00},            Alias{.key = Qt::Key_BracketLeft, .byte = 0x1b},
        Alias{.key = Qt::Key_3, .byte = 0x1b},            Alias{.key = Qt::Key_Backslash, .byte = 0x1c},
        Alias{.key = Qt::Key_4, .byte = 0x1c},            Alias{.key = Qt::Key_BracketRight, .byte = 0x1d},
        Alias{.key = Qt::Key_5, .byte = 0x1d},            Alias{.key = Qt::Key_AsciiCircum, .byte = 0x1e},
        Alias{.key = Qt::Key_6, .byte = 0x1e},            Alias{.key = Qt::Key_Underscore, .byte = 0x1f},
        Alias{.key = Qt::Key_Minus, .byte = 0x1f},        Alias{.key = Qt::Key_Slash, .byte = 0x1f},
        Alias{.key = Qt::Key_7, .byte = 0x1f},            Alias{.key = Qt::Key_Question, .byte = 0x7f},
        Alias{.key = Qt::Key_8, .byte = 0x7f},
    };
    const auto *const alias = std::ranges::find(kAliases, key, &Alias::key);
    return alias == kAliases.end() ? QByteArray{} : QByteArray(1, static_cast<char>(alias->byte));
}

// Option word movement and deletion for shell line editors (zsh/bash:
// \eb / \ef word left/right, \e\177 backward-kill-word, \eD forward kill,
// \e. last argument).
QByteArray optionWordKey(int key)
{
    switch (key) {
    case Qt::Key_Period:
        return "\x1b.";
    case Qt::Key_Left:
        return "\x1b" "b";
    case Qt::Key_Right:
        return "\x1b" "f";
    case Qt::Key_Backspace:
        return "\x1b\x7f";
    case Qt::Key_Delete:
        return "\x1b" "D";
    default:
        return {};
    }
}

// Kitty "disambiguate escape codes" (flag 1): Escape and modified
// Enter/Tab/Backspace and Ctrl/Alt text keys become CSI code;mods u. An
// Option press that composed a character (Option+2 = ™) sends that text, as
// kitty does by default on macOS.
QByteArray encodeKitty(int key, const QString &text, unsigned int mods)
{
    const auto csiU = [mods](int code) {
        QByteArray out = "\x1b[" + QByteArray::number(code);
        if (mods != 0) {
            out += ";" + modifierParam(mods);
        }
        return out + "u";
    };
    if (key == Qt::Key_Escape) {
        return csiU(27);
    }
    if (mods == 0) {
        return {};
    }
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return csiU(13);
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return csiU(9);
    case Qt::Key_Backspace:
        return csiU(127);
    default:
        break;
    }
    const bool composed = (mods & KeyMod::Control) == 0 && !text.isEmpty() && text.at(0).unicode() >= 0x80;
    // Text keys (Qt key codes are their ASCII value) with Ctrl or Alt:
    // the unshifted, lowercase code point.
    if (!composed && (mods & (KeyMod::Control | KeyMod::Alt)) != 0 && key >= 0x20 && key <= 0x7e) {
        return csiU(key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key);
    }
    return {};
}

QByteArray encodeEditingKey(int key, unsigned int mods, unsigned int modes)
{
    const bool shift = (mods & KeyMod::Shift) != 0;
    const bool alt = (mods & KeyMod::Alt) != 0;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // Codex and other TUIs bind LF (Ctrl+J) to insert a newline.
        if (shift && (mods & KeyMod::Control) == 0) {
            return "\n";
        }
        if ((modes & KeyMode::NewlineMode) != 0) {
            return alt ? "\x1b\r\n" : "\r\n";
        }
        return alt ? "\x1b\r" : "\r";
    case Qt::Key_Tab:
        return shift ? "\x1b[Z" : "\t";
    case Qt::Key_Backtab:
        return "\x1b[Z";
    case Qt::Key_Backspace:
        if ((mods & KeyMod::Control) != 0) {
            return "\x08";
        }
        return alt ? "\x1b\x7f" : "\x7f";
    case Qt::Key_Escape:
        return "\x1b";
    default:
        return {};
    }
}

} // namespace

QByteArray encodeKey(int key, const QString &text, unsigned int mods, unsigned int modes, unsigned int kittyFlags)
{
    const bool control = (mods & KeyMod::Control) != 0;
    const bool alt = (mods & KeyMod::Alt) != 0;

    if ((kittyFlags & 1u) != 0) {
        const QByteArray kitty = encodeKitty(key, text, mods);
        if (!kitty.isEmpty()) {
            return kitty;
        }
    }
    if ((modes & KeyMode::FromKeypad) != 0 && (modes & KeyMode::ApplicationKeypad) != 0 && mods == 0) {
        const auto *const keypad = std::ranges::find(kKeypadKeys, key, &KeypadKey::key);
        if (keypad != kKeypadKeys.end()) {
            const char *const prefix = (modes & KeyMode::Vt52) != 0 ? "\x1b?" : "\x1bO";
            return QByteArray(prefix) + static_cast<char>(keypad->final);
        }
    }
    if (alt && !control && (mods & KeyMod::Shift) == 0) {
        const QByteArray word = optionWordKey(key);
        if (!word.isEmpty()) {
            return word;
        }
    }
    const QByteArray editing = encodeEditingKey(key, mods, modes);
    if (!editing.isEmpty()) {
        return editing;
    }
    const auto *const special = std::ranges::find(kSpecialKeys, key, &SpecialKey::key);
    if (special != kSpecialKeys.end()) {
        return encodeSpecial(*special, mods, modes);
    }
    if (control) {
        const QByteArray byte = controlByte(key, text);
        return alt && !byte.isEmpty() ? "\x1b" + byte : byte;
    }
    // Option composes characters on macOS (Option+e = dead acute); send
    // whatever text the key produced.
    return text.toUtf8();
}

} // namespace konsoll
