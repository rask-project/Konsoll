#include "KeyEncoder.h"

#include <QList>
#include <Qt>

#include <algorithm>
#include <array>
#include <optional>

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

// --- Kitty keyboard protocol ---------------------------------------------

constexpr unsigned int kKittyDisambiguate = 1u;
constexpr unsigned int kKittyEventTypes = 2u;
constexpr unsigned int kKittyAlternates = 4u;
constexpr unsigned int kKittyAllKeys = 8u;
constexpr unsigned int kKittyText = 16u;

// A key with its kitty number and final byte ('u', '~' or a cursor letter).
// `legacy` keys (Enter, Tab, Backspace) keep their legacy bytes until "report
// all keys" (flag 8) is on; `modifier` keys are reported only with flag 8.
constexpr int kKittyNormal = 0;
constexpr int kKittyLegacy = 1;
constexpr int kKittyModifier = 2;

struct KittyKey {
    int key;
    int code;
    int final;
    int kind;
};

constexpr std::array kKittyFunctionalKeys{
    KittyKey{.key = Qt::Key_Escape, .code = 27, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Return, .code = 13, .final = 'u', .kind = kKittyLegacy},
    KittyKey{.key = Qt::Key_Enter, .code = 13, .final = 'u', .kind = kKittyLegacy},
    KittyKey{.key = Qt::Key_Tab, .code = 9, .final = 'u', .kind = kKittyLegacy},
    KittyKey{.key = Qt::Key_Backtab, .code = 9, .final = 'u', .kind = kKittyLegacy},
    KittyKey{.key = Qt::Key_Backspace, .code = 127, .final = 'u', .kind = kKittyLegacy},
    KittyKey{.key = Qt::Key_Insert, .code = 2, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Delete, .code = 3, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Left, .code = 1, .final = 'D', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Right, .code = 1, .final = 'C', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Up, .code = 1, .final = 'A', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Down, .code = 1, .final = 'B', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_PageUp, .code = 5, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_PageDown, .code = 6, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Home, .code = 1, .final = 'H', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_End, .code = 1, .final = 'F', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_CapsLock, .code = 57358, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_ScrollLock, .code = 57359, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_NumLock, .code = 57360, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Print, .code = 57361, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Pause, .code = 57362, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Menu, .code = 57363, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F1, .code = 1, .final = 'P', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F2, .code = 1, .final = 'Q', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F3, .code = 13, .final = '~', .kind = kKittyNormal}, // CSI R would read as CPR
    KittyKey{.key = Qt::Key_F4, .code = 1, .final = 'S', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F5, .code = 15, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F6, .code = 17, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F7, .code = 18, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F8, .code = 19, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F9, .code = 20, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F10, .code = 21, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F11, .code = 23, .final = '~', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_F12, .code = 24, .final = '~', .kind = kKittyNormal},
    // On macOS Qt::Key_Meta is the Control key and Qt::Key_Control Command.
    KittyKey{.key = Qt::Key_Shift, .code = 57441, .final = 'u', .kind = kKittyModifier},
    KittyKey{.key = Qt::Key_Meta, .code = 57442, .final = 'u', .kind = kKittyModifier},
    KittyKey{.key = Qt::Key_Alt, .code = 57443, .final = 'u', .kind = kKittyModifier},
    KittyKey{.key = Qt::Key_Control, .code = 57444, .final = 'u', .kind = kKittyModifier},
};

constexpr std::array kKittyKeypadKeys{
    KittyKey{.key = Qt::Key_0, .code = 57399, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_1, .code = 57400, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_2, .code = 57401, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_3, .code = 57402, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_4, .code = 57403, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_5, .code = 57404, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_6, .code = 57405, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_7, .code = 57406, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_8, .code = 57407, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_9, .code = 57408, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Period, .code = 57409, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Slash, .code = 57410, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Asterisk, .code = 57411, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Minus, .code = 57412, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Plus, .code = 57413, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Enter, .code = 57414, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Equal, .code = 57415, .final = 'u', .kind = kKittyNormal},
    KittyKey{.key = Qt::Key_Comma, .code = 57416, .final = 'u', .kind = kKittyNormal},
};

// The kitty code of a key: keypad and functional keys from the tables, F13+
// from the private-use range, text keys as their lowercase code point.
// `text` is set for keys that produce text.
std::optional<KittyKey> kittyKeyFor(int key, unsigned int modes, bool &textKey)
{
    textKey = false;
    if ((modes & KeyMode::FromKeypad) != 0) {
        const auto *const keypad = std::ranges::find(kKittyKeypadKeys, key, &KittyKey::key);
        if (keypad != kKittyKeypadKeys.end()) {
            return *keypad;
        }
    }
    const auto *const functional = std::ranges::find(kKittyFunctionalKeys, key, &KittyKey::key);
    if (functional != kKittyFunctionalKeys.end()) {
        return *functional;
    }
    if (key >= Qt::Key_F13 && key <= Qt::Key_F35) {
        return KittyKey{.key = key, .code = 57376 + (key - Qt::Key_F13), .final = 'u', .kind = kKittyNormal};
    }
    if (key >= 0x20 && key < 0x110000 && QChar::isPrint(static_cast<char32_t>(key))) {
        textKey = true;
        const auto lower = static_cast<int>(QChar::toLower(static_cast<char32_t>(key)));
        return KittyKey{.key = key, .code = lower, .final = 'u', .kind = kKittyNormal};
    }
    return std::nullopt;
}

// CSI code[:shifted] ; mods[:event] ; text u  (or the CSI 1;mods X / CSI n;mods ~
// forms of the functional keys).
QByteArray kittySequence(const KittyKey &kitty, int shifted, unsigned int mods, int eventType, const QString &text)
{
    QByteArray modifiers;
    if (mods != 0 || eventType != KeyEventType::Press || !text.isEmpty()) {
        modifiers = modifierParam(mods);
        if (eventType != KeyEventType::Press) {
            modifiers += ":" + QByteArray::number(eventType);
        }
    }
    if (kitty.final != 'u' && kitty.final != '~') {
        // Cursor keys and F1/F2/F4: CSI X, or CSI 1 ; mods X.
        return modifiers.isEmpty() ? QByteArray("\x1b[") + static_cast<char>(kitty.final)
                                   : "\x1b[1;" + modifiers + static_cast<char>(kitty.final);
    }
    QByteArray out = "\x1b[" + QByteArray::number(kitty.code);
    if (shifted != 0) {
        out += ":" + QByteArray::number(shifted);
    }
    if (!modifiers.isEmpty()) {
        out += ";" + modifiers;
    }
    if (!text.isEmpty()) {
        QByteArray codepoints;
        std::ranges::for_each(text.toUcs4(), [&codepoints](char32_t cp) {
            codepoints += (codepoints.isEmpty() ? "" : ":") + QByteArray::number(static_cast<int>(cp));
        });
        out += ";" + codepoints;
    }
    return out + static_cast<char>(kitty.final);
}

// The kitty encoding of an event, or an empty array when the legacy
// encoding applies (plain text, unmodified Enter/Tab/Backspace below flag 8)
// or nothing is sent (releases below flag 2, modifier keys below flag 8).
QByteArray encodeKitty(int key, const QString &text, unsigned int mods, unsigned int modes, unsigned int flags,
                       int eventType)
{
    const bool allKeys = (flags & kKittyAllKeys) != 0;
    const bool events = (flags & kKittyEventTypes) != 0;
    if (!events && eventType != KeyEventType::Press) {
        return {}; // without flag 2 a repeat is a press, a release nothing
    }
    bool textKey = false;
    const std::optional<KittyKey> kitty = kittyKeyFor(key, modes, textKey);
    if (!kitty || (kitty->kind == kKittyModifier && !allKeys)) {
        return {};
    }
    // Option composing a character (Option+2 = ™) sends that text, as kitty
    // does by default on macOS.
    const bool composed = (mods & KeyMod::Control) == 0 && !text.isEmpty() && text.at(0).unicode() >= 0x80;
    const bool plainText = textKey && ((mods & ~KeyMod::Shift) == 0 || composed);
    const bool legacyKey = kitty->kind == kKittyLegacy && mods == 0;
    if (!allKeys && eventType != KeyEventType::Release && (plainText || legacyKey)) {
        return {}; // legacy bytes (the text, CR, TAB, DEL)
    }
    if (!allKeys && legacyKey) {
        return {}; // Enter/Tab/Backspace releases need flag 8
    }
    const bool alternates = (flags & kKittyAlternates) != 0 && textKey && (mods & KeyMod::Shift) != 0;
    const int upper = static_cast<int>(QChar::toUpper(static_cast<char32_t>(kitty->code)));
    const int shifted = alternates && upper != kitty->code ? upper : 0;
    const bool withText = (flags & kKittyText) != 0 && allKeys && textKey && eventType != KeyEventType::Release;
    return kittySequence(*kitty, shifted, mods, eventType, withText ? text : QString{});
}

// The modifyOtherKeys code of a key: Enter / Tab / Backspace / Escape, or a
// text key's character with Shift applied (Ctrl+Shift+A = 65). 0 = none.
int otherKeyCode(int key, unsigned int mods, bool level2)
{
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return 13;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return 9;
    case Qt::Key_Backspace:
        return 127;
    case Qt::Key_Escape:
        return level2 ? 27 : 0;
    default:
        break;
    }
    if (key < 0x20 || key > 0x7e) {
        return 0;
    }
    const bool letter = key >= 'A' && key <= 'Z';
    return letter && (mods & KeyMod::Shift) == 0 ? key - 'A' + 'a' : key;
}

// modifyOtherKeys 1 encodes only what the legacy bytes cannot tell apart:
// Ctrl+Shift+letter, Ctrl+digit / punctuation without a control byte, and
// modified Enter / Tab / Backspace (Shift+Tab keeps its CSI Z).
bool legacyIsAmbiguous(int key, unsigned int mods)
{
    const bool control = (mods & KeyMod::Control) != 0;
    const bool textKey = key >= 0x20 && key <= 0x7e;
    const bool letter = key >= Qt::Key_A && key <= Qt::Key_Z;
    if (!textKey) {
        const bool shiftTab = (key == Qt::Key_Tab || key == Qt::Key_Backtab) && mods == KeyMod::Shift;
        return !shiftTab;
    }
    return control && (letter ? (mods & KeyMod::Shift) != 0 : controlByte(key, {}).isEmpty());
}

// xterm modifyOtherKeys: CSI 27 ; mods ; code ~ (or CSI code ; mods u with
// formatOtherKeys 1). Level 2 covers every Ctrl / Alt combination and the
// modified Enter / Tab / Backspace / Escape; level 1 see legacyIsAmbiguous.
QByteArray encodeOtherKeys(int key, const QString &text, unsigned int mods, unsigned int modes)
{
    const bool level2 = (modes & KeyMode::OtherKeys2) != 0;
    if ((!level2 && (modes & KeyMode::OtherKeys1) == 0) || mods == 0) {
        return {};
    }
    const int code = otherKeyCode(key, mods, level2);
    const bool textKey = key >= 0x20 && key <= 0x7e;
    const bool composed = !text.isEmpty() && text.at(0).unicode() >= 0x80 && (mods & KeyMod::Control) == 0;
    const bool shiftedText = textKey && (mods & (KeyMod::Control | KeyMod::Alt)) == 0;
    if (code == 0 || composed || shiftedText || (!level2 && !legacyIsAmbiguous(key, mods))) {
        return {}; // Shift alone on a text key just types it
    }
    const QByteArray modifiers = modifierParam(mods);
    if ((modes & KeyMode::OtherKeysCsiU) != 0) {
        return "\x1b[" + QByteArray::number(code) + ";" + modifiers + "u";
    }
    return "\x1b[27;" + modifiers + ";" + QByteArray::number(code) + "~";
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

QByteArray encodeKey(int key, const QString &text, unsigned int mods, unsigned int modes, unsigned int kittyFlags,
                     int eventType)
{
    const bool control = (mods & KeyMod::Control) != 0;
    const bool alt = (mods & KeyMod::Alt) != 0;

    if ((kittyFlags & (kKittyDisambiguate | kKittyEventTypes | kKittyAllKeys)) != 0) {
        const QByteArray kitty = encodeKitty(key, text, mods, modes, kittyFlags, eventType);
        if (!kitty.isEmpty()) {
            return kitty;
        }
    }
    if (eventType == KeyEventType::Release) {
        return {}; // legacy encodings have no releases
    }
    const QByteArray otherKeys = encodeOtherKeys(key, text, mods, modes);
    if (!otherKeys.isEmpty()) {
        return otherKeys;
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
