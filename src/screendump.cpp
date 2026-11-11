#include "screendump.h"

#include "term/Grapheme.h"
#include "term/Terminal.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstdio>
#include <iostream>
#include <print>
#include <ranges>
#include <span>
#include <string_view>

namespace konsoll
{

namespace
{

// One character summarizing a cell's style, for the --attrs view:
// B bold, I italic, U underline, R reverse, D dim, ~ curly underline,
// c colored (non-default fg/bg), '.' plain.
char styleMark(const Cell &cell)
{
    if ((cell.flags & CellFlag::Underline) != 0u) {
        return underlineStyle(cell) == UnderlineStyle::Curly ? '~' : 'U';
    }
    if ((cell.flags & CellFlag::Reverse) != 0u) {
        return 'R';
    }
    if ((cell.flags & CellFlag::Bold) != 0u) {
        return 'B';
    }
    if ((cell.attrs & CellAttr::Italic) != 0u) {
        return 'I';
    }
    if ((cell.flags & CellFlag::Dim) != 0u) {
        return 'D';
    }
    return cell.fg != 0 || cell.bg != 0 ? 'c' : '.';
}

QByteArray printable(const QByteArray &bytes)
{
    QByteArray out;
    std::ranges::for_each(bytes, [&out](char c) {
        const auto u = static_cast<unsigned char>(c);
        if (u == 0x1b) {
            out += "\\e";
        } else if (u == '\\') {
            out += "\\x5c";
        } else if (u < 0x20 || u >= 0x7f) {
            out += "\\x" + QByteArray::number(u, 16).rightJustified(2, '0');
        } else {
            out += c;
        }
    });
    return out;
}

// The replies that carry Konsoll's version (DA2 firmware, XTVERSION), with
// the version replaced by VERSION, so the fixtures don't change on a release.
QByteArray versionNeutral(QByteArray responses)
{
    responses.replace("\x1b[>41;" + QByteArray::number(reportedVersionNumber()) + ";0c", "\x1b[>41;VERSION;0c");
    responses.replace(QByteArray("\x1bP>|Konsoll ") + KONSOLL_VERSION + "\x1b\\", "\x1bP>|Konsoll VERSION\x1b\\");
    return responses;
}

} // namespace

QByteArray dumpTerminal(Terminal &term, bool attrs)
{
    QByteArray out;
    const Screen &screen = term.screen();
    std::ranges::for_each(std::views::iota(0, screen.rows()), [&](int row) {
        QString text;
        QByteArray marks;
        std::ranges::for_each(std::views::iota(0, screen.cols()), [&](int col) {
            const Cell &cell = screen.cellAt(row, col);
            if ((cell.flags & CellFlag::WideSpacer) != 0u) {
                return;
            }
            text += cellText(cell.ch);
            marks += styleMark(cell);
        });
        out += "|" + text.toUtf8() + "|";
        const int history = screen.inAltScreen() ? 0 : screen.scrollbackSize();
        if (const int size = screen.lineSize(history + row); size != Screen::SingleLine) {
            out += " size=" + QByteArray::number(size); // double width / height
        }
        out += "\n";
        if (attrs) {
            out += " " + marks + "\n";
        }
    });
    out += "--cursor " + QByteArray::number(screen.cursorRow()) + "," + QByteArray::number(screen.cursorCol()) +
           " alt=" + QByteArray::number(screen.inAltScreen() ? 1 : 0) +
           " visible=" + QByteArray::number(screen.cursorVisible() ? 1 : 0) + "\n";
    std::ranges::for_each(term.graphics().placements(), [&out](const GraphicsPlacement &p) {
        out += "--image id=" + QByteArray::number(p.imageId) + " anchor=" + QByteArray::number(p.anchor) +
               " col=" + QByteArray::number(p.column) + " size=" + QByteArray::number(p.columns) + "x" +
               QByteArray::number(p.rows) + "\n";
    });
    out += "--responses " + printable(versionNeutral(term.takeResponses())) + "\n";
    return out;
}

int runScreenDump(std::span<char *> args)
{
    if (args.size() < 4) {
        std::println(std::cerr, "usage: konsoll --screen-dump ROWS COLS [--attrs]");
        return 2;
    }
    const int rows = QByteArray(args[2]).toInt();
    const int cols = QByteArray(args[3]).toInt();
    const bool attrs = std::ranges::any_of(args, [](const char *arg) { return std::string_view(arg) == "--attrs"; });

    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) {
        return 1;
    }
    Terminal term(std::max(rows, 2), std::max(cols, 2), QStringLiteral("screendump"));
    term.write(input.readAll());
    std::print(std::cout, "{}", dumpTerminal(term, attrs).toStdString());
    return 0;
}

} // namespace konsoll
