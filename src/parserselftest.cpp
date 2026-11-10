#include "selftest.h"

#include "KeyEncoder.h"
#include "screendump.h"
#include "term/Grapheme.h"
#include "term/Terminal.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QByteArray>
#include <QImage>
#include <QString>

namespace konsoll
{

namespace
{

// A fresh terminal fed `bytes`; most checks only need a small screen.
Terminal feed(const QByteArray &bytes, int rows = 4, int cols = 10)
{
    Terminal term(rows, cols, QStringLiteral("selftest"));
    term.write(bytes);
    return term;
}

// The text of screen row `row`, trailing blanks trimmed.
QString rowText(const Terminal &term, int row)
{
    QString text;
    const Screen &screen = term.screen();
    for (int col = 0; col < screen.cols(); ++col) {
        const char32_t cp = static_cast<char32_t>(screen.cellAt(row, col).ch);
        text += QString::fromUcs4(&cp, 1);
    }
    return text.trimmed();
}

// Stage 1: the parser consumes every sequence whole.
void parserStateMachine(int &failures)
{
    selfTestCheck(failures, rowText(feed("a\x1bP+q544e\x1b\\b"), 0) == QStringLiteral("ab"),
                  "parser: DCS (XTGETTCAP) body does not leak as text");
    selfTestCheck(failures, rowText(feed("a\x1b_Gi=1;AAAA\x1b\\b\x1b^pm\x1b\\c\x1bXsos\x1b\\d"), 0) ==
                                QStringLiteral("abcd"),
                  "parser: APC / PM / SOS strings are dropped");

    {
        const Terminal term = feed("\x1b[?1049h\x1b[?1049s");
        selfTestCheck(failures, term.screen().inAltScreen(), "parser: CSI ? 1049 s (XTSAVE) does not leave the alt screen");
    }
    {
        const Terminal term = feed("\x1b[?2026h\x1b[?2026$p");
        selfTestCheck(failures, term.synchronizedUpdates(), "parser: CSI ? 2026 $ p (DECRQM) does not reset the mode");
    }
    {
        // U+011C is C4 9C in UTF-8: 0x9c inside an OSC is text, not ST.
        const Terminal term = feed("\x1b]2;\xc4\x9cx\x07");
        selfTestCheck(failures, term.title() == QStringLiteral(u"Ĝx"), "parser: UTF-8 0x9c byte inside OSC is text");
    }
    selfTestCheck(failures, rowText(feed("abc\x1b[2\rCx"), 0) == QStringLiteral("abx"),
                  "parser: C0 control inside CSI executes, CSI completes");
    selfTestCheck(failures, rowText(feed("\x1b[3\x18" "Ab"), 0) == QStringLiteral("Ab"), "parser: CAN aborts a CSI");
    {
        const Terminal term = feed("\x1b]0;t\x1b[1mZ");
        selfTestCheck(failures,
                      term.title() == QStringLiteral("t") &&
                          (term.screen().cellAt(0, 0).flags & CellFlag::Bold) != 0u && term.screen().cellAt(0, 0).ch == 'Z',
                      "parser: ESC inside OSC ends it and starts a new sequence");
    }
    selfTestCheck(failures, rowText(feed("a\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;"
                                         "27;28;29;30;31;32;33;34;35;36;37;38;39;40;41;42;43;44;45;46;47;48;49;50;"
                                         "51;52;53;54;55;56;57;58;59;60;61;62;63;64;65;66;67;68;69;70;71;72;73;74;"
                                         "75;76;77;78;79;80;81;82;83;84;85;86;87;88;89;90;91;92;93;94;95;96;97;98xb"),
                                    0) == QStringLiteral("ab"),
                  "parser: long parameter lists are consumed");
}

// Stage 2: SGR with colon sub-parameters and every rendered attribute.
void sgrAttributes(int &failures)
{
    const auto cell = [](const QByteArray &bytes) { return feed(bytes + "x").screen().cellAt(0, 0); };
    const auto flag = [](const Cell &c, unsigned int f) { return (c.flags & f) != 0u; };
    const auto attr = [](const Cell &c, unsigned int a) { return (c.attrs & a) != 0u; };

    {
        const Cell c = cell("\x1b[4:3m");
        selfTestCheck(failures, flag(c, CellFlag::Underline) && underlineStyle(c) == UnderlineStyle::Curly &&
                                    !attr(c, CellAttr::Italic),
                      "sgr: 4:3 is a curly underline, not underline + italic");
    }
    {
        const Cell c = cell("\x1b[1;4m\x1b[4:0m");
        selfTestCheck(failures, flag(c, CellFlag::Bold) && !flag(c, CellFlag::Underline),
                      "sgr: 4:0 removes the underline and keeps other attributes");
    }
    selfTestCheck(failures, underlineStyle(cell("\x1b[21m")) == UnderlineStyle::Double &&
                                underlineStyle(cell("\x1b[4:4m")) == UnderlineStyle::Dotted &&
                                underlineStyle(cell("\x1b[4:5m")) == UnderlineStyle::Dashed,
                  "sgr: double (21, 4:2), dotted (4:4) and dashed (4:5) underlines");
    {
        const Cell c = cell("\x1b[1m\x1b[58;2;255;0;0m");
        selfTestCheck(failures, c.ul == 0x80ff0000u && flag(c, CellFlag::Bold) && !flag(c, CellFlag::Dim),
                      "sgr: 58;2;r;g;b sets the underline color without dim or reset");
    }
    selfTestCheck(failures, cell("\x1b[58:5:9m").ul == 10u && cell("\x1b[58:5:9m\x1b[59m").ul == 0u,
                  "sgr: 58:5:n indexed underline color, 59 resets it");
    selfTestCheck(failures, cell("\x1b[38:2::1:2:3m").fg == 0x80010203u && cell("\x1b[38:2:1:2:3m").fg == 0x80010203u &&
                                cell("\x1b[48:5:200m").bg == 201u,
                  "sgr: colon forms 38:2::r:g:b, 38:2:r:g:b and 48:5:n");
    selfTestCheck(failures, cell("\x1b[38;5;1;1m").fg == 2u && flag(cell("\x1b[38;5;1;1m"), CellFlag::Bold),
                  "sgr: parameters after a semicolon color still apply");
    selfTestCheck(failures, attr(cell("\x1b[3m"), CellAttr::Italic) && !attr(cell("\x1b[3;23m"), CellAttr::Italic),
                  "sgr: italic on (3) / off (23)");
    selfTestCheck(failures, flag(cell("\x1b[8m"), CellFlag::Invisible) && !flag(cell("\x1b[8;28m"), CellFlag::Invisible),
                  "sgr: hidden on (8) / off (28)");
    selfTestCheck(failures, attr(cell("\x1b[5m"), CellAttr::Blink) && !attr(cell("\x1b[5;25m"), CellAttr::Blink),
                  "sgr: blink on (5) / off (25)");
    selfTestCheck(failures, attr(cell("\x1b[53m"), CellAttr::Overline) && !attr(cell("\x1b[53;55m"), CellAttr::Overline),
                  "sgr: overline on (53) / off (55)");
    {
        const Cell c = cell("\x1b[1;3;4:3;58:5:1;31m\x1b[;1m");
        selfTestCheck(failures, flag(c, CellFlag::Bold) && !attr(c, CellAttr::Italic) && !flag(c, CellFlag::Underline) &&
                                    c.ul == 0u && c.fg == 0u,
                      "sgr: an empty first parameter is SGR 0");
    }
}

bool cursorAt(const Terminal &term, int row, int col)
{
    return term.screen().cursorRow() == row && term.screen().cursorCol() == col;
}

// Stage 3: cursor movement, REP, tabs, saved cursor.
void cursorSequences(int &failures)
{
    selfTestCheck(failures, cursorAt(feed("ab\x1b[2E"), 2, 0) && cursorAt(feed("\x1b[3;5H\x1b[F"), 1, 0),
                  "csi: CNL (E) / CPL (F) move lines and return");
    selfTestCheck(failures, cursorAt(feed("\x1b[5`"), 0, 4) && cursorAt(feed("ab\x1b[2a"), 0, 4) &&
                                cursorAt(feed("\x1b[2e"), 2, 0),
                  "csi: HPA (`), HPR (a), VPR (e)");
    selfTestCheck(failures, rowText(feed("-\x1b[4b"), 0) == QStringLiteral("-----"), "csi: REP (b) repeats the last character");
    selfTestCheck(failures, rowText(feed("abcdefgh\r\x1b[2C \x1b[5b"), 0) == QStringLiteral("ab"),
                  "csi: REP of spaces erases the rest of a prompt line");
    selfTestCheck(failures, cursorAt(feed("\x1b[2I", 4, 20), 0, 16) && cursorAt(feed("\x1b[18G\x1b[Z", 4, 20), 0, 16),
                  "csi: CHT (I) / CBT (Z) use tab stops");
    selfTestCheck(failures, cursorAt(feed("\x1b[3g\x1b[5G\x1bH\r\t", 4, 20), 0, 4),
                  "csi: TBC clears tab stops, HTS sets one");
    selfTestCheck(failures, rowText(feed("ab\x1b[s\x1b[3;1Hz\x1b[ux"), 0) == QStringLiteral("abx"),
                  "csi: SCOSC (s) / SCORC (u) save and restore the cursor");
    {
        const Terminal term = feed("\x1b[1m\x1b" "7\x1b[0m\x1b" "8x");
        selfTestCheck(failures, (term.screen().cellAt(0, 0).flags & CellFlag::Bold) != 0u,
                      "esc: DECSC / DECRC save and restore the pen");
    }
    selfTestCheck(failures, cursorAt(feed("\x1b[2;4r\x1b[3;1H\x1b[10A"), 1, 0) &&
                                cursorAt(feed("\x1b[2;3r\x1b[2;1H\x1b[10B"), 2, 0),
                  "csi: CUU / CUD stop at the scroll margins");
    selfTestCheck(failures, cursorAt(feed("\x1b[3;1Hx\x1b[2;3r"), 0, 0), "csi: DECSTBM homes the cursor");
    selfTestCheck(failures, cursorAt(feed("abc\x1b[L"), 0, 0) && cursorAt(feed("abc\x1b[M"), 0, 0),
                  "csi: IL / DL return to the left margin");
}

// Stage 3: modes, screens, character sets.
void screenModes(int &failures)
{
    {
        const Terminal term = feed("hello\x1b[?1049h\x1b[2;2Hvim\x1b[?1049l");
        selfTestCheck(failures, !term.screen().inAltScreen() && cursorAt(term, 0, 5) && rowText(term, 0) == QStringLiteral("hello"),
                      "mode 1049: leaving restores the main screen cursor");
    }
    {
        Terminal term = feed("1\r\n2\r\n3\r\n4\r\n", 2, 10);
        const int before = term.screen().scrollbackSize();
        term.write("\x1b[?1049hx\x1b[?1049l");
        selfTestCheck(failures, before > 0 && term.screen().scrollbackSize() == before,
                      "mode 1049: the alternate screen keeps the main scrollback");
    }
    {
        const Terminal term = feed("top\x1b[1;2r\x1b[4;1Hx\n");
        selfTestCheck(failures, cursorAt(term, 3, 1) && rowText(term, 0) == QStringLiteral("top"),
                      "lf: below the scroll region the cursor does not scroll it");
    }
    selfTestCheck(failures, rowText(feed("abcdefghij\x1b[1;8H\x1b[20@"), 0) == QStringLiteral("abcdefg") &&
                                rowText(feed("abcdefghij\x1b[1;3H\x1b[20P"), 0) == QStringLiteral("ab"),
                  "csi: ICH / DCH with a count past the right margin");
    selfTestCheck(failures, rowText(feed("\x1b(0qx\x1b(Bq"), 0) == QStringLiteral(u"─│q") &&
                                rowText(feed("\x1b)0\x0eq\x0fq"), 0) == QStringLiteral(u"─q"),
                  "charset: DEC special graphics via G0 and via G1 + SO/SI");
    selfTestCheck(failures, cursorAt(feed("\x1b[2;3r\x1b[?6h\x1b[1;1Hx"), 1, 1), "mode 6: origin mode addresses the region");
    {
        const Terminal term = feed("\x1b[?7labcdefghijXY");
        selfTestCheck(failures, rowText(term, 0) == QStringLiteral("abcdefghiY") && rowText(term, 1).isEmpty(),
                      "mode 7: without autowrap the last column is overwritten");
    }
    selfTestCheck(failures, rowText(feed("abc\r\x1b[4hX"), 0) == QStringLiteral("Xabc"), "mode 4: insert mode (IRM)");
    selfTestCheck(failures, rowText(feed("\x1b#8"), 3) == QStringLiteral("EEEEEEEEEE"), "esc: DECALN fills the screen");
    {
        const Terminal term = feed("\x1b[?6h\x1b[4h\x1b[1m\x1b[!px");
        selfTestCheck(failures, !term.screen().originMode() && !term.screen().insertMode() &&
                                    (term.screen().cellAt(0, 0).flags & CellFlag::Bold) == 0u,
                      "csi: DECSTR soft reset");
    }
    selfTestCheck(failures, !feed("\x1b[?1049h\x1b" "c").screen().inAltScreen(), "esc: RIS leaves the alternate screen");
    selfTestCheck(failures, cursorAt(feed("a\x1b[20h\n"), 1, 0), "mode 20: LNM makes LF return");
    selfTestCheck(failures, feed("\x1b[?5h").reverseVideo() && !feed("\x1b[?5h\x1b[?5l").reverseVideo(),
                  "mode 5: reverse video");
}

// The replies a terminal fed `bytes` queues for the PTY.
QByteArray replies(const QByteArray &bytes, int rows = 4, int cols = 10)
{
    Terminal term(rows, cols, QStringLiteral("selftest"));
    term.write(bytes);
    return term.takeResponses();
}

// Stage 4a: query replies.
void queryReplies(int &failures)
{
    selfTestCheck(failures, replies("\x1b[c") == "\x1b[?64;4;6;21;22;28c" && replies("\x1b[0c") == "\x1b[?64;4;6;21;22;28c",
                  "reply: DA1");
    selfTestCheck(failures, replies("\x1b[>c").startsWith("\x1b[>41;") && replies("\x1b[=c") == "\x1bP!|00000000\x1b\\",
                  "reply: DA2 / DA3");
    selfTestCheck(failures, replies("\x1b[>q").startsWith("\x1bP>|Konsoll "), "reply: XTVERSION");
    selfTestCheck(failures, replies("\x1b]11;?\x07\x1b[5n") == "\x1b]11;rgb:1414/1414/1414\x1b\\\x1b[0n",
                  "reply: OSC 11 then DSR 5, in order");
    selfTestCheck(failures, replies("\x1b[2;3H\x1b[6n\x1b[?6n") == "\x1b[2;3R\x1b[?2;3R", "reply: CPR / DECXCPR");
    selfTestCheck(failures, replies("\x1b[2;4r\x1b[?6h\x1b[2;1H\x1b[6n") == "\x1b[2;1R",
                  "reply: CPR is origin-relative in origin mode");
    selfTestCheck(failures, replies("\x1b[?996n") == "\x1b[?997;1n", "reply: color scheme (dark default)");
    selfTestCheck(failures, replies("\x1b[?2026h\x1b[?2026$p\x1b[?1049$p\x1b[?9999$p") ==
                                "\x1b[?2026;1$y\x1b[?1049;2$y\x1b[?9999;0$y",
                  "reply: DECRQM for DEC modes (set, reset, unknown)");
    selfTestCheck(failures, replies("\x1b[4h\x1b[4$p\x1b[20$p") == "\x1b[4;1$y\x1b[20;2$y", "reply: DECRQM for ANSI modes");
    selfTestCheck(failures, replies("\x1b[1;3;4:3;38;5;196;48;2;1;2;3m\x1bP$qm\x1b\\") ==
                                "\x1bP1$r0;1;3;4:3;38:5:196;48:2::1:2:3m\x1b\\",
                  "reply: DECRQSS m (SGR)");
    selfTestCheck(failures, replies("\x1b[2;3r\x1bP$qr\x1b\\\x1b[5 q\x1bP$q q\x1b\\\x1bP$qz\x1b\\") ==
                                "\x1bP1$r2;3r\x1b\\\x1bP1$r5 q\x1b\\\x1bP0$r\x1b\\",
                  "reply: DECRQSS r, SP q and an invalid request");
    // "Tc" = 5463, "Co" = 436f, "xx" = 7878.
    selfTestCheck(failures, replies("\x1bP+q5463;436f;7878\x1b\\") ==
                                "\x1bP1+r5463\x1b\\\x1bP1+r436f=323536\x1b\\\x1bP0+r7878\x1b\\",
                  "reply: XTGETTCAP boolean, string and unknown capabilities");
    selfTestCheck(failures, replies("\x1b[18t\x1b[16t", 5, 20) == "\x1b[8;5;20t\x1b[6;16;8t", "reply: XTWINOPS 18t / 16t");
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b]2;one\x07\x1b[22;0t\x1b]2;two\x07\x1b[23;0t");
        selfTestCheck(failures, term.title() == QStringLiteral("one"), "reply: XTWINOPS 22/23 push and pop the title");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b[?2048h");
        const QByteArray enabled = term.takeResponses();
        term.resize(6, 12);
        selfTestCheck(failures, enabled == "\x1b[48;4;10;64;80t" && term.takeResponses() == "\x1b[48;6;12;96;96t",
                      "report: in-band resize (2048) on enable and on resize");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b[?2031h");
        term.setDefaultColors(0xff000000u, 0xffffffffu);
        selfTestCheck(failures, term.takeResponses() == "\x1b[?997;2n", "report: color scheme change (2031)");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b[>1u\x1b[>5u\x1b[?u");
        const QByteArray nested = term.takeResponses();
        term.write("\x1b[<u\x1b[?u");
        const QByteArray popped = term.takeResponses();
        term.write("\x1b[<u\x1b[?u");
        selfTestCheck(failures, nested == "\x1b[?5u" && popped == "\x1b[?1u" && term.takeResponses() == "\x1b[?0u",
                      "kitty: keyboard flags are a push/pop stack");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b[?1000h\x1b[?1006h");
        const QByteArray sgr = term.mouseReport(0, 1, 2, 0, 0);
        term.write("\x1b[?1006l\x1b[?1015h");
        const QByteArray urxvt = term.mouseReport(0, 1, 2, 0, 0);
        selfTestCheck(failures, sgr == "\x1b[<0;3;2M" && urxvt == "\x1b[32;3;2M", "mouse: SGR (1006) and urxvt (1015) encodings");
    }
}

// Stage 4b: cursor style and focus reporting modes.
void cursorStyle(int &failures)
{
    const Terminal bar = feed("\x1b[5 q");
    const Terminal underline = feed("\x1b[4 q");
    const Terminal block = feed("\x1b[2 q");
    selfTestCheck(failures, bar.cursorShape() == 2 && bar.cursorBlinking() && underline.cursorShape() == 1 &&
                                !underline.cursorBlinking() && block.cursorShape() == 0 && !block.cursorBlinking(),
                  "cursor: DECSCUSR bar / underline / block and blinking");
    selfTestCheck(failures, !feed("").cursorBlinking() && feed("\x1b[?12h").cursorBlinking() &&
                                !feed("\x1b[?12h\x1b[2 q").cursorBlinking(),
                  "cursor: ?12 blinks the default style, DECSCUSR overrides it");
    selfTestCheck(failures, feed("\x1b[?1004h").focusReporting() && !feed("\x1b[?1004h\x1b[?1004l").focusReporting() &&
                                replies("\x1b[?1004h\x1b[?1004$p\x1b[?12$p") == "\x1b[?1004;1$y\x1b[?12;2$y",
                  "focus: ?1004 focus reporting mode");
}

// Stage 4c: key encoding.
void keyEncoding(int &failures)
{
    const auto key = [](int k, const QString &text = {}, unsigned int mods = 0, unsigned int modes = 0,
                        unsigned int kitty = 0) { return encodeKey(k, text, mods, modes, kitty); };
    selfTestCheck(failures, key(Qt::Key_F1) == "\x1bOP" && key(Qt::Key_F5) == "\x1b[15~" && key(Qt::Key_F12) == "\x1b[24~" &&
                                key(Qt::Key_Insert) == "\x1b[2~",
                  "keys: F1 / F5 / F12 / Insert");
    selfTestCheck(failures, key(Qt::Key_Up, {}, KeyMod::Control) == "\x1b[1;5A" &&
                                key(Qt::Key_Delete, {}, KeyMod::Shift) == "\x1b[3;2~" &&
                                key(Qt::Key_F3, {}, KeyMod::Alt | KeyMod::Shift) == "\x1b[1;4R",
                  "keys: modifier parameters on cursor and function keys");
    selfTestCheck(failures, key(Qt::Key_Home) == "\x1b[H" && key(Qt::Key_Home, {}, 0, KeyMode::ApplicationCursor) == "\x1bOH" &&
                                key(Qt::Key_Left, {}, 0, KeyMode::ApplicationCursor) == "\x1bOD",
                  "keys: Home and arrows in normal and application cursor mode");
    selfTestCheck(failures, key(Qt::Key_Tab, {}, KeyMod::Shift) == "\x1b[Z" && key(Qt::Key_Backtab, {}, KeyMod::Shift) == "\x1b[Z",
                  "keys: Shift+Tab is CBT");
    selfTestCheck(failures, key(Qt::Key_Space, {}, KeyMod::Control) == QByteArray(1, '\0') &&
                                key(Qt::Key_BracketRight, {}, KeyMod::Control) == "\x1d" &&
                                key(Qt::Key_Slash, {}, KeyMod::Control) == "\x1f" &&
                                key(Qt::Key_C, {}, KeyMod::Control) == "\x03" &&
                                key(Qt::Key_C, {}, KeyMod::Control | KeyMod::Alt) == "\x1b\x03",
                  "keys: Ctrl letters and punctuation aliases, Ctrl+Alt prefixes ESC");
    selfTestCheck(failures, key(Qt::Key_Left, {}, KeyMod::Alt) == "\x1b" "b" &&
                                key(Qt::Key_E, QStringLiteral(u"é"), KeyMod::Alt) == "\xc3\xa9",
                  "keys: Option word movement, Option composition keeps its text");
    selfTestCheck(failures, key(Qt::Key_5, QStringLiteral("5"), 0, KeyMode::FromKeypad | KeyMode::ApplicationKeypad) == "\x1bOu" &&
                                key(Qt::Key_5, QStringLiteral("5"), 0, KeyMode::FromKeypad) == "5",
                  "keys: application keypad");
    selfTestCheck(failures, key(Qt::Key_Escape, {}, 0, 0, 1) == "\x1b[27u" && key(Qt::Key_C, {}, KeyMod::Control, 0, 1) == "\x1b[99;5u" &&
                                key(Qt::Key_Return, {}, KeyMod::Shift, 0, 1) == "\x1b[13;2u" &&
                                key(Qt::Key_Return, {}, 0, 0, 1) == "\r" && key(Qt::Key_A, QStringLiteral("a"), 0, 0, 1) == "a" &&
                                key(Qt::Key_Up, {}, 0, 0, 1) == "\x1b[A" &&
                                key(Qt::Key_X, QStringLiteral("x"), KeyMod::Alt, 0, 1) == "\x1b[120;3u" &&
                                key(Qt::Key_2, QStringLiteral(u"\u2122"), KeyMod::Alt, 0, 1) == "\xe2\x84\xa2",
                  "keys: kitty disambiguate (flag 1)");
    selfTestCheck(failures, key(Qt::Key_Return, {}, 0, KeyMode::NewlineMode) == "\r\n",
                  "keys: Return sends CR LF in LNM");
    selfTestCheck(failures, key(Qt::Key_Return, {}, KeyMod::Shift) == "\n" && key(Qt::Key_Backspace) == "\x7f",
                  "keys: legacy Shift+Enter newline and Backspace");
}

// Stage 4d: OSC 8 hyperlinks, OSC color sets/queries, alternate scroll.
void linksAndColors(int &failures)
{
    {
        const Terminal term = feed("\x1b]8;;https://example.com\x1b\\ab\x1b]8;;\x1b\\c");
        const Screen &screen = term.screen();
        const unsigned short id = screen.cellAt(0, 0).link;
        selfTestCheck(failures, id != 0 && screen.cellAt(0, 1).link == id && screen.cellAt(0, 2).link == 0 &&
                                    term.linkTarget(id) == QStringLiteral("https://example.com"),
                      "osc 8: cells carry the hyperlink until it is closed");
    }
    {
        const Terminal term = feed("\x1b]8;id=1;https://a\x1b\\x\x1b]8;;\x1b\\ \x1b]8;id=1;https://a\x1b\\y\x1b[0mz");
        const Screen &screen = term.screen();
        selfTestCheck(failures, screen.cellAt(0, 0).link != 0 && screen.cellAt(0, 2).link == screen.cellAt(0, 0).link &&
                                    screen.cellAt(0, 3).link == screen.cellAt(0, 0).link,
                      "osc 8: the same id reuses the link, SGR 0 keeps it");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b]4;1;rgb:ff/00/00;2;#00ff00\x1b\\");
        const bool set = term.paletteColor(1) == 0xffff0000u && term.paletteColor(2) == 0xff00ff00u;
        term.write("\x1b]4;1;?\x1b\\");
        const QByteArray query = term.takeResponses();
        term.write("\x1b]104;1\x1b\\");
        const bool resetOne = term.paletteColor(1) != 0xffff0000u && term.paletteColor(2) == 0xff00ff00u;
        term.write("\x1b]104\x1b\\");
        selfTestCheck(failures, set && query == "\x1b]4;1;rgb:ffff/0000/0000\x1b\\" && resetOne &&
                                    term.paletteColor(2) != 0xff00ff00u && term.colorsDirty(),
                      "osc 4 / 104: set, query and reset palette entries");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b]10;#102030\x07\x1b]10;?\x07\x1b]11;rgb:f/f/f\x07");
        const QByteArray query = term.takeResponses();
        const bool set = term.foregroundOverride() == 0xff102030u && term.backgroundOverride() == 0xffffffffu;
        term.write("\x1b]110\x07\x1b]111\x07");
        selfTestCheck(failures, set && query == "\x1b]10;rgb:1010/2020/3030\x1b\\" && term.foregroundOverride() == 0 &&
                                    term.backgroundOverride() == 0,
                      "osc 10 / 11 / 110 / 111: set, query and reset the default colors");
    }
    selfTestCheck(failures, feed("").alternateScroll() && !feed("\x1b[?1007l").alternateScroll() &&
                                replies("\x1b[?1007$p") == "\x1b[?1007;1$y",
                  "mode 1007: alternate scroll is on by default");
}

// Fixes found running vttest through tools/vtdrive.py.
void vttestFindings(int &failures)
{
    {
        const Terminal term = feed("junk\x1b[5;5r\x1b[3;3H\x1b[?3l");
        selfTestCheck(failures, rowText(term, 0).isEmpty() && cursorAt(term, 0, 0) && term.screen().scrollTop() == 0,
                      "vttest: DECCOLM clears, homes and resets the margins");
    }
    selfTestCheck(failures, rowText(feed("keep\x1b[?95h\x1b[?3h"), 0) == QStringLiteral("keep"),
                  "vttest: DECNCSM (?95) keeps the screen on DECCOLM");
    selfTestCheck(failures, rowText(feed("\x1b-A\x0e!#\x0f"), 0) == QStringLiteral(u"¡£") &&
                                rowText(feed("\x1b(K{|}~"), 0) == QStringLiteral("{|}~") &&
                                rowText(feed("\x1b[?42h\x1b(K{|}~"), 0) == QStringLiteral(u"äöüß") &&
                                rowText(feed("\x1b[?42h\x1b(%6[\\]"), 0) == QStringLiteral(u"ÃÇÕ"),
                  "charset: 96-character Latin-1 in G1, NRC sets only in DECNRCM");
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b[?2l\x1bY\"%x\x1bZ\x1b<\x1b[c");
        selfTestCheck(failures, term.screen().cellAt(2, 5).ch == 'x' && !term.vt52Mode() &&
                                    term.takeResponses() == "\x1b/Z\x1b[?64;4;6;21;22;28c",
                      "vt52: ESC Y addressing, identify, ESC < back to ANSI");
    }
    selfTestCheck(failures, replies("\x1b[0x\x1b[1x") == "\x1b[2;1;1;128;128;1;0x\x1b[3;1;1;128;128;1;0x", "reply: DECREQTPARM");
    selfTestCheck(failures, rowText(feed("+\x1b[1b\x1b[10b\r\n \x1b[2b+"), 0) == QStringLiteral("++") &&
                                rowText(feed("+\x1b[1b\x1b[10b\r\n \x1b[2b+"), 1) == QStringLiteral("+") &&
                                rowText(feed("ab\x1b[1;31m\x1b[2b"), 0) == QStringLiteral("abbb"),
                  "csi: REP repeats the last character (across SGR), and only once");
    selfTestCheck(failures, rowText(feed("abcdef\x1b[2 @"), 0) == QStringLiteral("cdef") &&
                                rowText(feed("abcdef\x1b[2 A"), 0) == QStringLiteral("abcdef") &&
                                feed("abcdef\x1b[2 A").screen().cellAt(0, 2).ch == 'a',
                  "csi: SL / SR scroll the region sideways");
    selfTestCheck(failures, rowText(feed("\x1b[1\"qab\x1b[0\"qcd\x1b[1;1H\x1b[?2K"), 0) == QStringLiteral("ab") &&
                                rowText(feed("\x1bVab\x1bWcd\x1b[1;1H\x1b[2K"), 0) == QStringLiteral("ab") &&
                                rowText(feed("\x1bVab\x1bWcd\x1b[6h\x1b[1;1H\x1b[2K"), 0).isEmpty(),
                  "protect: DECSCA vs DECSEL, SPA vs EL, ERM erases protected cells");
}

// Kitty keyboard protocol, all progressive enhancements.
void kittyKeyboard(int &failures)
{
    const auto key = [](int k, const QString &text, unsigned int mods, unsigned int flags,
                        int event = KeyEventType::Press, unsigned int modes = 0) {
        return encodeKey(k, text, mods, modes, flags, event);
    };
    const QString a = QStringLiteral("a");
    selfTestCheck(failures, key(Qt::Key_A, a, 0, 8) == "\x1b[97u" && key(Qt::Key_Return, {}, 0, 8) == "\x1b[13u" &&
                                key(Qt::Key_Shift, {}, KeyMod::Shift, 8) == "\x1b[57441;2u" &&
                                key(Qt::Key_Shift, {}, KeyMod::Shift, 1).isEmpty(),
                  "kitty 8: text, Enter and modifier keys as escape codes");
    selfTestCheck(failures, key(Qt::Key_A, a, 0, 2, KeyEventType::Release) == "\x1b[97;1:3u" &&
                                key(Qt::Key_C, {}, KeyMod::Control, 3, KeyEventType::Repeat) == "\x1b[99;5:2u" &&
                                key(Qt::Key_Up, {}, 0, 2) == "\x1b[A" &&
                                key(Qt::Key_Up, {}, 0, 2, KeyEventType::Release) == "\x1b[1;1:3A" &&
                                key(Qt::Key_A, a, 0, 2, KeyEventType::Repeat) == "a",
                  "kitty 2: repeat and release events");
    selfTestCheck(failures, key(Qt::Key_Return, {}, 0, 2, KeyEventType::Release).isEmpty() &&
                                key(Qt::Key_Return, {}, 0, 10, KeyEventType::Release) == "\x1b[13;1:3u" &&
                                key(Qt::Key_A, a, 0, 0, KeyEventType::Release).isEmpty(),
                  "kitty 2: Enter release needs flag 8; legacy mode has no releases");
    selfTestCheck(failures, key(Qt::Key_A, QStringLiteral("A"), KeyMod::Shift, 12) == "\x1b[97:65;2u",
                  "kitty 4: shifted alternate key");
    selfTestCheck(failures, key(Qt::Key_A, a, 0, 24) == "\x1b[97;1;97u", "kitty 16: associated text");
    selfTestCheck(failures, key(Qt::Key_5, QStringLiteral("5"), 0, 1, KeyEventType::Press, KeyMode::FromKeypad) == "\x1b[57404u" &&
                                key(Qt::Key_F3, {}, 0, 1) == "\x1b[13~" && key(Qt::Key_F13, {}, 0, 1) == "\x1b[57376u",
                  "kitty: keypad codes, F3 as CSI 13 ~, F13+ private codes");
}

// Kitty graphics protocol and sixel.
void graphicsProtocols(int &failures)
{
    // 2x2 RGBA: red, green, blue, white.
    const QByteArray rgba("\xff\x00\x00\xff\x00\xff\x00\xff\x00\x00\xff\xff\xff\xff\xff\xff", 16);
    const QByteArray apc = "\x1b_G";
    const QByteArray st = "\x1b\\";
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write(apc + "a=T,f=32,s=2,v=2,i=7;" + rgba.toBase64() + st);
        const GraphicsStore &graphics = term.graphics();
        const GraphicsImage *const image = graphics.image(7);
        selfTestCheck(failures, term.takeResponses() == "\x1b_Gi=7;OK\x1b\\" && image != nullptr &&
                                    image->pixels->pixelColor(1, 0) == QColor(0, 255, 0) &&
                                    graphics.placements().size() == 1 && term.screen().cursorCol() == 1,
                      "kitty graphics: transmit and display raw RGBA, reply OK, move the cursor");
        term.write(apc + "a=d,d=i,i=7" + st);
        const bool placementGone = term.graphics().placements().empty() && term.graphics().image(7) != nullptr;
        term.write(apc + "a=d,d=I,i=7" + st);
        selfTestCheck(failures, placementGone && term.graphics().image(7) == nullptr,
                      "kitty graphics: d=i keeps the image data, d=I frees it");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        const QByteArray encoded = rgba.toBase64();
        term.write(apc + "a=t,f=32,s=2,v=2,I=3,m=1;" + encoded.left(8) + st);
        term.write(apc + "m=0;" + encoded.mid(8) + st);
        const QByteArray stored = term.takeResponses();
        term.write(apc + "a=p,I=3,c=4,r=2,q=1" + st);
        const GraphicsStore &graphics = term.graphics();
        selfTestCheck(failures, stored.startsWith("\x1b_Gi=") && stored.contains(",I=3;OK") && graphics.placements().size() == 1 &&
                                    graphics.placements().front().columns == 4 && graphics.placements().front().rows == 2 &&
                                    term.takeResponses().isEmpty() && term.screen().cursorRow() == 1,
                      "kitty graphics: chunked transmit by number, put with c/r, quiet");
    }
    {
        QImage png(3, 1, QImage::Format_RGB32);
        png.fill(Qt::blue);
        QByteArray pngBytes;
        QBuffer buffer(&pngBytes);
        buffer.open(QIODevice::WriteOnly);
        png.save(&buffer, "PNG");
        const QByteArray zlib = qCompress(rgba).mid(4); // qCompress = 4-byte size + zlib stream
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write(apc + "a=t,f=100,i=1;" + pngBytes.toBase64() + st);
        term.write(apc + "a=t,f=32,s=2,v=2,o=z,i=2;" + zlib.toBase64() + st);
        term.write(apc + "a=q,i=9,f=32,s=2,v=2;" + rgba.toBase64() + st);
        term.write(apc + "a=q,i=10,f=100;" + QByteArray("junk").toBase64() + st);
        const QByteArray replies = term.takeResponses();
        selfTestCheck(failures, term.graphics().image(1) != nullptr && term.graphics().image(1)->pixels->width() == 3 &&
                                    term.graphics().image(2) != nullptr && term.graphics().image(9) == nullptr &&
                                    replies.contains("\x1b_Gi=9;OK") && replies.contains("\x1b_Gi=10;EBADPNG"),
                      "kitty graphics: PNG, zlib-compressed data, queries (valid and invalid)");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1bPq#0;2;100;0;0#0~~\x1b\\");
        const GraphicsStore &graphics = term.graphics();
        const bool placed = graphics.placements().size() == 1;
        const GraphicsImage *const image = placed ? graphics.image(graphics.placements().front().imageId) : nullptr;
        selfTestCheck(failures, image != nullptr && image->pixels->width() == 2 && image->pixels->height() == 6 &&
                                    image->pixels->pixelColor(1, 5) == QColor(255, 0, 0) && term.screen().cursorRow() == 1,
                      "sixel: decode color registers and data, cursor below the image");
        term.write("\r\n\r\n\r\n\r\n");
        const long long anchor = term.graphics().placements().front().anchor;
        selfTestCheck(failures, anchor < term.screen().rowAnchor(0), "graphics: images scroll with the text");
        term.write("\x1b[2J");
        selfTestCheck(failures, !term.graphics().placements().empty(), "graphics: ED 2 keeps images scrolled off screen");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b[?1049h" + apc + "a=T,f=32,s=2,v=2;" + rgba.toBase64() + st);
        const bool placed = term.graphics().placements().size() == 1;
        term.write("\x1b[?1049l");
        selfTestCheck(failures, placed && term.graphics().placements().empty(),
                      "graphics: leaving the alternate screen removes its images");
        term.write(apc + "a=T,f=32,s=2,v=2;" + rgba.toBase64() + st + "\x1b[2J");
        selfTestCheck(failures, term.graphics().placements().empty(), "graphics: ED 2 removes images on the screen");
    }
    selfTestCheck(failures, replies("\x1b[?1;1;0S\x1b[?2;1;0S", 4, 10) == "\x1b[?1;0;256S\x1b[?2;0;80;64S",
                  "reply: XTSMGRAPHICS color registers and sixel geometry");
}

// Requests the session answers by the user's Security settings.
void sessionRequests(int &failures)
{
    Terminal term(4, 10, QStringLiteral("selftest"));
    term.write("\x1b]52;c;?\x07\x1b]52;;?\x07\x1b[21t\x1b[20t\x1b[8;30;100t\x1b[2t\x1b]52;c;aGk=\x07");
    const std::vector<TerminalRequest> requests = term.takeRequests();
    const auto kindAt = [&requests](std::size_t i) { return i < requests.size() ? requests[i].kind : -1; };
    selfTestCheck(failures, requests.size() == 6 && kindAt(0) == TerminalRequest::ClipboardRead && requests[0].data == "c" &&
                                requests[1].data == "c" && kindAt(2) == TerminalRequest::TitleReport && requests[2].a == 0 &&
                                requests[3].a == 1 && kindAt(4) == TerminalRequest::WindowOp && requests[4].a == 8 &&
                                requests[4].b == 30 && requests[4].c == 100 && requests[5].a == 2 &&
                                term.clipboardText() == QStringLiteral("hi") && term.takeResponses().isEmpty(),
                  "requests: OSC 52 reads, title reports and window operations wait for the session");
}

// Left/right margins (DECSLRM) and VT420 rectangular operations.
void marginsAndRectangles(int &failures)
{
    // Margins at columns 3..6 (1-based).
    const QByteArray margins = "\x1b[?69h\x1b[3;6s";
    selfTestCheck(failures, rowText(feed("\x1b[?69h\x1b[3;6s\x1b[1;3Habcdef"), 0) == QStringLiteral("abcd") &&
                                rowText(feed("\x1b[?69h\x1b[3;6s\x1b[1;3Habcdef"), 1) == QStringLiteral("ef"),
                  "margins: autowrap at the right margin continues at the left margin");
    selfTestCheck(failures, cursorAt(feed(margins + "\x1b[1;5H\r"), 0, 2) && cursorAt(feed(margins + "\x1b[1;1H\r"), 0, 0),
                  "margins: CR goes to the left margin from inside it");
    selfTestCheck(failures, cursorAt(feed(margins + "\x1b[1;4H\x1b[20C"), 0, 5) && cursorAt(feed(margins + "\x1b[1;4H\x1b[20D"), 0, 2),
                  "margins: CUF / CUB stop at the margins");
    selfTestCheck(failures, cursorAt(feed(margins + "\x1b[?6h\x1b[1;1H"), 0, 2) && cursorAt(feed(margins + "\x1b[?6h\x1b[1;99H"), 0, 5),
                  "margins: origin mode addresses inside the margins");
    {
        const Terminal term = feed("0123456789\r\n0123456789\x1b[?69h\x1b[3;6s\x1b[1;3H\x1b[L");
        selfTestCheck(failures, rowText(term, 0) == QStringLiteral("01    6789") && rowText(term, 1) == QStringLiteral("0123456789"),
                      "margins: IL only shifts the margin columns");
    }
    selfTestCheck(failures, rowText(feed("0123456789\x1b[?69h\x1b[3;6s\x1b[1;3H\x1b[P"), 0) == QStringLiteral("01345 6789") &&
                                rowText(feed("0123456789\x1b[?69h\x1b[3;6s\x1b[1;3H\x1b[@"), 0) == QStringLiteral("01 2346789"),
                  "margins: DCH / ICH stop at the right margin");
    selfTestCheck(failures, rowText(feed("0123456789\x1b[?69h\x1b[3;6s\x1b[1;3H\x1b[2'}"), 0) == QStringLiteral("01  236789") &&
                                rowText(feed("0123456789\x1b[?69h\x1b[3;6s\x1b[1;3H\x1b[2'~"), 0) == QStringLiteral("0145  6789"),
                  "csi: DECIC / DECDC inside the margins");
    selfTestCheck(failures, rowText(feed("0123456789\x1b[?69h\x1b[3;6s\x1b[1;3H\x1b" "6"), 0) == QStringLiteral("01 2346789") &&
                                rowText(feed("0123456789\x1b[?69h\x1b[3;6s\x1b[1;6H\x1b" "9"), 0) == QStringLiteral("01345 6789"),
                  "esc: DECBI / DECFI scroll the margin columns");
    selfTestCheck(failures, replies(margins + "\x1b[?69$p\x1bP$qs\x1b\\") == "\x1b[?69;1$y\x1bP1$r3;6s\x1b\\" &&
                                rowText(feed("\x1b[?69l\x1b[3;6sx\x1b[ux"), 0) == QStringLiteral("x"),
                  "margins: DECRQM / DECRQSS, and CSI s is SCOSC without DECLRMM");
    {
        const Terminal term = feed("\x1b[42;2;2;3;5$x");
        selfTestCheck(failures, rowText(term, 1) == QStringLiteral("****") && rowText(term, 2) == QStringLiteral("****") &&
                                    rowText(term, 0).isEmpty(),
                      "rect: DECFRA fills a rectangle");
    }
    selfTestCheck(failures, rowText(feed("abcdef\r\nabcdef\x1b[1;2;2;3$z"), 0) == QStringLiteral("a  def") &&
                                rowText(feed("abcdef\r\nabcdef\x1b[1;2;2;3$z"), 1) == QStringLiteral("a  def"),
                  "rect: DECERA erases a rectangle");
    selfTestCheck(failures, rowText(feed("ab\x1b[1\"qcd\x1b[0\"qef\x1b[1;1;1;6${"), 0) == QStringLiteral("cd"),
                  "rect: DECSERA keeps DECSCA-protected cells");
    selfTestCheck(failures, rowText(feed("abc\x1b[1;1;1;3;1;3;5;1$v"), 2) == QStringLiteral("abc"),
                  "rect: DECCRA copies a rectangle");
    {
        const Terminal term = feed("abcdef\r\nabcdef\x1b[2*x\x1b[1;2;2;3;1;7$r");
        const auto flags = [&term](int row, int col) { return term.screen().cellAt(row, col).flags; };
        selfTestCheck(failures, (flags(0, 1) & CellFlag::Bold) != 0u && (flags(1, 2) & CellFlag::Reverse) != 0u &&
                                    (flags(0, 3) & CellFlag::Bold) == 0u && (flags(1, 0) & CellFlag::Bold) == 0u,
                      "rect: DECCARA in rectangle mode (DECSACE 2)");
    }
    {
        const Terminal term = feed("abcdef\r\nabcdef\x1b[1;5;2;2;1$r");
        const auto bold = [&term](int row, int col) { return (term.screen().cellAt(row, col).flags & CellFlag::Bold) != 0u; };
        selfTestCheck(failures, bold(0, 5) && bold(1, 0) && !bold(1, 2) && !bold(0, 3),
                      "rect: DECCARA in stream mode by default");
    }
    selfTestCheck(failures, (feed("ab\x1b[1;1;1;1$t").screen().cellAt(0, 0).flags & CellFlag::Reverse) != 0u &&
                                (feed("ab\x1b[1;1;1;1;7$t\x1b[1;1;1;1;7$t").screen().cellAt(0, 0).flags & CellFlag::Reverse) == 0u &&
                                (feed("ab\x1b[1;1;1;1;7$t").screen().cellAt(0, 1).flags & CellFlag::Reverse) == 0u,
                  "rect: DECRARA toggles attributes (0 = all of them)");
    // 'A' + 'B' = 131 -> -131 & 0xffff = 0xff7d
    selfTestCheck(failures, replies("AB\x1b[5;1;1;1;1;2*y") == "\x1bP5!~FF7D\x1b\\", "rect: DECRQCRA checksum");
}

// DEC double-width / double-height lines.
void lineSizes(int &failures)
{
    {
        const Terminal term = feed("\x1b#6abcdefgh");
        selfTestCheck(failures, term.screen().lineSize(0) == Screen::DoubleWidthLine && rowText(term, 0) == QStringLiteral("abcde") &&
                                    rowText(term, 1) == QStringLiteral("fgh"),
                      "lines: DECDWL halves the columns (wraps at 5 of 10)");
    }
    selfTestCheck(failures, cursorAt(feed("\x1b#6\x1b[1;9H"), 0, 4) && cursorAt(feed("abcdefgh\x1b#6"), 0, 4),
                  "lines: the cursor stays in the visible half");
    selfTestCheck(failures, feed("\x1b#3\r\n\x1b#4").screen().lineSize(0) == Screen::DoubleHeightTop &&
                                feed("\x1b#3\r\n\x1b#4").screen().lineSize(1) == Screen::DoubleHeightBottom &&
                                feed("\x1b#6\x1b#5").screen().lineSize(0) == Screen::SingleLine,
                  "lines: DECDHL top / bottom and DECSWL");
    {
        const Terminal term = feed("\x1b[4;1H\x1b#6\n\n", 4, 10);
        const int history = term.screen().scrollbackSize(); // lineSize() takes lineAt() indexes
        selfTestCheck(failures, term.screen().lineSize(history + 1) == Screen::DoubleWidthLine &&
                                    term.screen().lineSize(history + 3) == Screen::SingleLine,
                      "lines: sizes scroll with their rows");
    }
    selfTestCheck(failures, feed("\x1b#6\x1b[2J").screen().lineSize(0) == Screen::SingleLine &&
                                feed("\x1b#6\x1b#8").screen().lineSize(0) == Screen::SingleLine &&
                                feed("\x1b#6\x1b[?1049h\x1b[?1049l").screen().lineSize(0) == Screen::DoubleWidthLine,
                  "lines: ED 2 and DECALN reset sizes, the alternate screen keeps the main ones");
}

// xterm modifyOtherKeys (XTMODKEYS / XTFMTKEYS / XTQMODKEYS).
void modifyOtherKeys(int &failures)
{
    selfTestCheck(failures, feed("\x1b[>4;2m").modifyOtherKeys() == 2 && feed("\x1b[>4;1m").modifyOtherKeys() == 1 &&
                                feed("\x1b[>4;2m\x1b[>4n").modifyOtherKeys() == 0 && feed("\x1b[>4;1f").otherKeysCsiU() &&
                                replies("\x1b[>4;2m\x1b[?4m") == "\x1b[>4;2m",
                  "modifyOtherKeys: set, reset, format and query");
    const auto key = [](int k, const QString &text, unsigned int mods, unsigned int modes) {
        return encodeKey(k, text, mods, modes, 0);
    };
    const unsigned int one = KeyMode::OtherKeys1;
    const unsigned int two = KeyMode::OtherKeys2;
    selfTestCheck(failures, key(Qt::Key_A, {}, KeyMod::Control, one) == "\x01" &&
                                key(Qt::Key_A, {}, KeyMod::Control | KeyMod::Shift, one) == "\x1b[27;6;65~" &&
                                key(Qt::Key_1, {}, KeyMod::Control, one) == "\x1b[27;5;49~" &&
                                key(Qt::Key_Return, {}, KeyMod::Control, one) == "\x1b[27;5;13~" &&
                                key(Qt::Key_Return, {}, KeyMod::Shift, one) == "\x1b[27;2;13~" &&
                                key(Qt::Key_Tab, {}, KeyMod::Shift, one) == "\x1b[Z",
                  "modifyOtherKeys 1: only keys the legacy encoding cannot tell apart");
    selfTestCheck(failures, key(Qt::Key_A, {}, KeyMod::Control, two) == "\x1b[27;5;97~" &&
                                key(Qt::Key_X, QStringLiteral("x"), KeyMod::Alt, two) == "\x1b[27;3;120~" &&
                                key(Qt::Key_A, QStringLiteral("A"), KeyMod::Shift, two) == "A" &&
                                key(Qt::Key_2, QStringLiteral(u"™"), KeyMod::Alt, two) == "\xe2\x84\xa2" &&
                                key(Qt::Key_A, {}, KeyMod::Control, two | KeyMode::OtherKeysCsiU) == "\x1b[97;5u",
                  "modifyOtherKeys 2: every Ctrl / Alt key, CSI u format");
}

// Horizontal wheel buttons and highlight tracking (?1001).
void mouseExtras(int &failures)
{
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b[?1000h\x1b[?1006h");
        selfTestCheck(failures, term.mouseReport(66, 0, 0, 5, 0) == "\x1b[<66;1;1M" && term.mouseReport(67, 1, 2, 6, 0) == "\x1b[<67;3;2M",
                      "mouse: horizontal wheel is buttons 6 / 7 (66 / 67)");
    }
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write("\x1b[?1001h");
        const QByteArray press = term.mouseReport(0, 1, 2, 0, 0);
        term.write("\x1b[1;3;2;1;4T");
        const QByteArray release = term.mouseReport(0, 1, 4, 1, 0);
        const bool scrolled = feed("a\x1b[T").screen().cellAt(1, 0).ch == 'a';
        selfTestCheck(failures, press == "\x1b[M #\"" && release == QByteArray("\x1b[t%\"") && scrolled &&
                                    replies("\x1b[?1001h\x1b[?1001$p") == "\x1b[?1001;1$y",
                      "mouse: ?1001 highlight tracking, CSI T with one parameter is still SD");
    }
}

// Option as Meta (setting, ?1036 / ?1039).
void optionAsMeta(int &failures)
{
    const auto key = [](int k, const QString &text, unsigned int mods, unsigned int modes, unsigned int kitty = 0) {
        return encodeKey(k, text, mods, modes, kitty);
    };
    const unsigned int meta = KeyMode::OptionMeta;
    selfTestCheck(failures, key(Qt::Key_F, QStringLiteral(u"ƒ"), KeyMod::Alt, meta) == "\x1b" "f" &&
                                key(Qt::Key_F, QStringLiteral(u"ƒ"), KeyMod::Alt | KeyMod::Shift, meta) == "\x1b" "F" &&
                                key(Qt::Key_E, {}, KeyMod::Alt, meta) == "\x1b" "e" &&
                                key(Qt::Key_F, QStringLiteral(u"ƒ"), KeyMod::Alt, 0) == "\xc6\x92",
                  "meta: Option+key sends ESC + key (dead keys too) instead of the composed text");
    selfTestCheck(failures, key(Qt::Key_Left, {}, KeyMod::Alt, meta) == "\x1b" "b" &&
                                key(Qt::Key_2, QStringLiteral(u"™"), KeyMod::Alt, meta, 1) == "\x1b[50;3u",
                  "meta: word movement stays, kitty reports Option as Alt");
    selfTestCheck(failures, feed("\x1b[?1036h").metaSendsEscape() && feed("\x1b[?1039h").metaSendsEscape() &&
                                !feed("\x1b[?1036h\x1b[?1036l").metaSendsEscape() &&
                                replies("\x1b[?1036h\x1b[?1036$p") == "\x1b[?1036;1$y",
                  "meta: ?1036 / ?1039 metaSendsEscape / altSendsEscape");
}

// Grapheme clusters (mode 2027).
void graphemeClusters(int &failures)
{
    const QByteArray family = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7";
    {
        const Terminal term = feed("\x1b[?2027h" + family + "x");
        const Cell &cell = term.screen().cellAt(0, 0);
        selfTestCheck(failures, cell.ch >= kClusterBase && (cell.flags & CellFlag::Wide) != 0u &&
                                    cellText(cell.ch) == QString::fromUtf8(family) && term.screen().cellAt(0, 2).ch == 'x',
                      "grapheme: a ZWJ family is one wide cell");
    }
    selfTestCheck(failures, feed(family + "x").screen().cursorCol() == 7,
                  "grapheme: without mode 2027 a ZWJ sequence keeps the per-code-point widths");
    selfTestCheck(failures, feed("\x1b[?2027h\xf0\x9f\x87\xa7\xf0\x9f\x87\xb7\xf0\x9f\x87\xa7\xf0\x9f\x87\xb7").screen().cursorCol() == 4,
                  "grapheme: regional indicators pair into flags");
    selfTestCheck(failures, feed("\x1b[?2027h\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd" "x").screen().cursorCol() == 3 &&
                                feed("\x1b[?2027h\xe2\x9d\xa4\xef\xb8\x8f" "x").screen().cursorCol() == 3,
                  "grapheme: emoji modifier and VS16 clusters are wide");
    {
        const Terminal term = feed("\x1b[?2027he\xcc\xa3\xcc\x81x");
        // e + dot below composes to U+1EB9; the acute stays in the cluster.
        selfTestCheck(failures, cellCodePoints(term.screen().cellAt(0, 0).ch) == std::u32string(U"\u1EB9\u0301") &&
                                    term.screen().cellAt(0, 1).ch == 'x',
                      "grapheme: a letter keeps several combining marks in one cell");
    }
    selfTestCheck(failures, replies("\x1b[?2027$p\x1b[?2027h\x1b[?2027$p") == "\x1b[?2027;2$y\x1b[?2027;1$y" &&
                                feed("\x1b[?2027h\xf0\x9f\x87\xa7\xf0\x9f\x87\xb7\x1b[2b").screen().cursorCol() == 6,
                  "grapheme: DECRQM ?2027 and REP of a cluster");
}

// tests/fixtures: recorded program output replayed against its expected
// screen (see tools/regress.py).
void fixtureRegressions(int &failures)
{
    const QDir dir(QStringLiteral(KONSOLL_SOURCE_DIR "/tests/fixtures"));
    static const QRegularExpression pattern(QStringLiteral(R"(^(.+)-(\d+)x(\d+)\.raw$)"));
    const QStringList raws = dir.entryList({QStringLiteral("*.raw")}, QDir::Files, QDir::Name);
    std::ranges::for_each(raws, [&](const QString &name) {
        const QRegularExpressionMatch match = pattern.match(name);
        QFile raw(dir.filePath(name));
        QFile expected(dir.filePath(name.chopped(4) + QStringLiteral(".expected")));
        if (!match.hasMatch() || !raw.open(QIODevice::ReadOnly) || !expected.open(QIODevice::ReadOnly)) {
            return;
        }
        Terminal term(match.captured(2).toInt(), match.captured(3).toInt(), QStringLiteral("screendump"));
        term.write(raw.readAll());
        const QByteArray what = "fixture: " + name.toUtf8() + " matches its .expected screen";
        selfTestCheck(failures, dumpTerminal(term, true) == expected.readAll(), what.constData());
    });
}

} // namespace

void runParserSelfTest(int &failures)
{
    parserStateMachine(failures);
    sgrAttributes(failures);
    cursorSequences(failures);
    screenModes(failures);
    queryReplies(failures);
    cursorStyle(failures);
    keyEncoding(failures);
    linksAndColors(failures);
    vttestFindings(failures);
    kittyKeyboard(failures);
    graphicsProtocols(failures);
    sessionRequests(failures);
    marginsAndRectangles(failures);
    lineSizes(failures);
    modifyOtherKeys(failures);
    graphemeClusters(failures);
    optionAsMeta(failures);
    mouseExtras(failures);
    fixtureRegressions(failures);
}

} // namespace konsoll
