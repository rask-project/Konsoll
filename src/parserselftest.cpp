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

} // namespace

void runParserSelfTest(int &failures)
{
    parserStateMachine(failures);
    sgrAttributes(failures);
}

} // namespace konsoll
