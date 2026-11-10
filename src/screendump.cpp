#include "screendump.h"

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

} // namespace

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

    const Screen &screen = term.screen();
    std::ranges::for_each(std::views::iota(0, screen.rows()), [&](int row) {
        QString text;
        QByteArray marks;
        std::ranges::for_each(std::views::iota(0, screen.cols()), [&](int col) {
            const Cell &cell = screen.cellAt(row, col);
            if ((cell.flags & CellFlag::WideSpacer) != 0u) {
                return;
            }
            const auto cp = static_cast<char32_t>(cell.ch);
            text += QString::fromUcs4(&cp, 1);
            marks += styleMark(cell);
        });
        std::println(std::cout, "|{}|", text.toStdString());
        if (attrs) {
            std::println(std::cout, " {}", marks.toStdString());
        }
    });
    std::println(std::cout, "--cursor {},{} alt={} visible={}", screen.cursorRow(), screen.cursorCol(),
                 screen.inAltScreen() ? 1 : 0, screen.cursorVisible() ? 1 : 0);
    std::println(std::cout, "--responses {}", printable(term.takeResponses()).toStdString());
    return 0;
}

} // namespace konsoll
