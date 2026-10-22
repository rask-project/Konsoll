#include "Screen.h"

#include <algorithm>
#include <ranges>

namespace konsoll
{

namespace
{
Cell blankCell()
{
    return Cell{.fg = 0, .bg = 0, .ch = QChar(' '), .flags = CellFlag::None};
}

std::vector<Line> makeGrid(int rows, int cols)
{
    return std::vector<Line>(rows, Line(cols, blankCell()));
}

// Resize preserving the top-left content.
std::vector<Line> resizeGrid(const std::vector<Line> &old, int rows, int cols)
{
    auto grid = makeGrid(rows, cols);
    const int copyRows = std::min<int>(rows, static_cast<int>(old.size()));
    for (int r = 0; r < copyRows; ++r) {
        const int copyCols = std::min<int>(cols, static_cast<int>(old[r].size()));
        std::copy(old[r].begin(), old[r].begin() + copyCols, grid[r].begin());
    }
    return grid;
}
} // namespace

Screen::Screen(int rows, int cols) : m_rows(rows), m_cols(cols), m_scrollBottom(rows - 1)
{
    m_main = makeGrid(rows, cols);
}

void Screen::resize(int rows, int cols)
{
    if (rows <= 0 || cols <= 0) {
        return;
    }
    m_rows = rows;
    m_cols = cols;
    m_main = resizeGrid(m_main, rows, cols);
    if (m_inAlt) {
        m_alt = resizeGrid(m_alt, rows, cols);
    }
    m_cursorRow = std::clamp(m_cursorRow, 0, rows - 1);
    m_cursorCol = std::clamp(m_cursorCol, 0, cols - 1);
    m_scrollTop = 0;
    m_scrollBottom = rows - 1;
    m_pendingWrap = false;
}

void Screen::moveCursor(int row, int col)
{
    m_cursorRow = std::clamp(row, 0, m_rows - 1);
    m_cursorCol = std::clamp(col, 0, m_cols - 1);
    m_pendingWrap = false;
}

void Screen::saveCursor()
{
    m_savedRow = m_cursorRow;
    m_savedCol = m_cursorCol;
}

void Screen::restoreCursor()
{
    m_cursorRow = std::clamp(m_savedRow, 0, m_rows - 1);
    m_cursorCol = std::clamp(m_savedCol, 0, m_cols - 1);
    m_pendingWrap = false;
}

void Screen::showCursor(bool visible)
{
    m_cursorVisible = visible;
}

const Cell &Screen::cellAt(int row, int col) const
{
    static const Cell kBlank = blankCell();
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols) {
        return kBlank;
    }
    return grid()[row][col];
}

void Screen::putChar(QChar ch, unsigned int fg, unsigned int bg, unsigned char flags) // NOLINT: fg/bg are inherently swappable
{
    if (m_pendingWrap) {
        m_cursorCol = 0;
        lineFeed();
        m_pendingWrap = false;
    }
    Cell &c = grid()[m_cursorRow][m_cursorCol];
    c.ch = ch;
    c.fg = fg;
    c.bg = bg;
    c.flags = flags;
    if (m_cursorCol + 1 >= m_cols) {
        m_pendingWrap = true;
    } else {
        ++m_cursorCol;
    }
}

void Screen::lineFeed()
{
    if (m_cursorRow >= m_scrollBottom) {
        scrollUpOne(m_scrollTop, m_scrollBottom);
    } else {
        ++m_cursorRow;
    }
}

void Screen::carriageReturn()
{
    m_cursorCol = 0;
    m_pendingWrap = false;
}

void Screen::backspace()
{
    if (m_cursorCol > 0) {
        --m_cursorCol;
    }
    m_pendingWrap = false;
}

void Screen::tab()
{
    const int next = ((m_cursorCol / 8) + 1) * 8;
    m_cursorCol = std::min(next, m_cols - 1);
    m_pendingWrap = false;
}

void Screen::reverseIndex()
{
    if (m_inAlt) {
        return;
    }
    if (m_cursorRow <= m_scrollTop) {
        scrollDownOne(m_scrollTop, m_scrollBottom);
    } else {
        --m_cursorRow;
    }
}

void Screen::eraseInDisplay(int mode)
{
    auto &g = grid();
    if (mode == 3) {
        clearScrollback();
    }

    if (mode == 0) {
        // Cursor to end of screen.
        for (int c = m_cursorCol; c < m_cols; ++c) {
            g[m_cursorRow][c] = blankCell();
        }
        for (int r = m_cursorRow + 1; r < m_rows; ++r) {
            g[r].assign(m_cols, blankCell());
        }
    } else if (mode == 1) {
        // Start of screen to cursor.
        for (int r = 0; r < m_cursorRow; ++r) {
            g[r].assign(m_cols, blankCell());
        }
        for (int c = 0; c <= m_cursorCol; ++c) {
            g[m_cursorRow][c] = blankCell();
        }
    } else if (mode == 2) {
        g.assign(m_rows, Line(m_cols, blankCell()));
    }
}

void Screen::eraseInLine(int mode)
{
    auto &g = grid();
    if (mode == 0) {
        for (int c = m_cursorCol; c < m_cols; ++c) {
            g[m_cursorRow][c] = blankCell();
        }
    } else if (mode == 1) {
        for (int c = 0; c <= m_cursorCol; ++c) {
            g[m_cursorRow][c] = blankCell();
        }
    } else if (mode == 2) {
        g[m_cursorRow].assign(m_cols, blankCell());
    }
}

void Screen::scrollUp(int count)
{
    for (int i = 0; i < count; ++i) {
        scrollUpOne(m_scrollTop, m_scrollBottom);
    }
}

void Screen::scrollDown(int count)
{
    for (int i = 0; i < count; ++i) {
        scrollDownOne(m_scrollTop, m_scrollBottom);
    }
}

void Screen::setScrollRegion(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    // 1-based inclusive; clamp to the grid.
    int t = std::clamp(top - 1, 0, m_rows - 1);
    int b = std::clamp(bottom - 1, 0, m_rows - 1);
    if (t > b) {
        std::swap(t, b);
    }
    m_scrollTop = t;
    m_scrollBottom = b;
    moveCursor(t, 0);
}

void Screen::resetScrollRegion()
{
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
}

void Screen::deleteColumns(int count)
{
    if (count <= 0) {
        return;
    }
    auto &g = grid();
    Line &row = g[m_cursorRow];
    if (count >= m_cols) {
        row.assign(m_cols, blankCell());
        return;
    }
    // Shift [cursorCol .. end-count] left by `count`.
    for (int c = m_cursorCol; c + count < m_cols; ++c) {
        row[c] = row[c + count];
    }
    std::fill(row.end() - count, row.end(), blankCell());
}

void Screen::insertColumns(int count)
{
    if (count <= 0) {
        return;
    }
    auto &g = grid();
    Line &row = g[m_cursorRow];
    if (count >= m_cols) {
        row.assign(m_cols, blankCell());
        return;
    }
    // Shift [cursorCol .. end] right by `count` (overflow dropped).
    for (int c = m_cols - 1; c >= m_cursorCol + count; --c) {
        row[c] = row[c - count];
    }
    for (int c = m_cursorCol; c < m_cursorCol + count; ++c) {
        row[c] = blankCell();
    }
}

const Line &Screen::scrollbackLine(int index) const
{
    static const Line kEmpty;
    if (index < 0 || index >= static_cast<int>(m_scrollback.size())) {
        return kEmpty;
    }
    auto it = m_scrollback.begin();
    std::advance(it, index);
    return *it;
}

void Screen::clearScrollback()
{
    m_scrollback.clear();
}

void Screen::enterAltScreen()
{
    if (m_inAlt) {
        return;
    }
    m_mainSaved = m_main;
    m_alt = makeGrid(m_rows, m_cols);
    m_inAlt = true;
    clearScrollback();
    moveCursor(0, 0);
}

void Screen::leaveAltScreen()
{
    if (!m_inAlt) {
        return;
    }
    m_main = m_mainSaved;
    m_inAlt = false;
    moveCursor(0, 0);
}

void Screen::fullReset()
{
    auto &g = grid();
    g.assign(m_rows, Line(m_cols, blankCell()));
    moveCursor(0, 0);
    m_pendingWrap = false;
    resetScrollRegion();
}

void Screen::scrollUpOne(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    auto &g = grid();
    if (top == 0 && !m_inAlt) {
        pushScrollback(g[top]);
    }
    for (int r = top; r < bottom; ++r) {
        g[r] = g[r + 1];
    }
    blankLine(bottom);
}

void Screen::scrollDownOne(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    auto &g = grid();
    for (int r = bottom; r > top; --r) {
        g[r] = g[r - 1];
    }
    blankLine(top);
}

void Screen::blankLine(int row)
{
    grid()[row].assign(m_cols, blankCell());
}

void Screen::pushScrollback(const Line &line)
{
    m_scrollback.push_back(line);
    while (static_cast<int>(m_scrollback.size()) > kMaxScrollback) {
        m_scrollback.pop_front();
    }
}

} // namespace konsoll
