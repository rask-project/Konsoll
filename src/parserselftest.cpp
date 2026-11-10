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

} // namespace

void runParserSelfTest(int &failures)
{
    parserStateMachine(failures);
}

} // namespace konsoll
