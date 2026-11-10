#include "selftest.h"

#include "term/Terminal.h"

#include <QByteArray>
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

} // namespace

void runParserSelfTest(int &failures)
{
    parserStateMachine(failures);
    sgrAttributes(failures);
    cursorSequences(failures);
    screenModes(failures);
}

} // namespace konsoll
