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

// Keep the wrapped flags for rows whose content survives the resize; new
// rows are hard breaks. `flags` is resized to `rows`.
void carryWrappedFlags(std::vector<bool> &flags, int rows)
{
    const int keep = std::min(rows, static_cast<int>(flags.size()));
    std::vector<bool> old(flags);
    flags.assign(rows, false);
    for (int r = 0; r < keep; ++r) {
        flags[r] = old[r];
    }
}

// Adjust a grid to have exactly `rows` rows, each `width` cols wide (new rows
// blank, excess dropped). Used before reflow so the grid has the right row
// count at the OLD width; reflow then re-wraps to the new width. Padding the
// rows to the new width first would leave padding in the middle of logical
// lines.
void resizeRowsTo(std::vector<Line> &grid, int rows, int width)
{
    while (static_cast<int>(grid.size()) < rows) {
        grid.emplace_back(width, blankCell());
    }
    if (static_cast<int>(grid.size()) > rows) {
        grid.resize(rows);
    }
}
} // namespace

Screen::Screen(int rows, int cols) : m_rows(rows), m_cols(cols), m_scrollBottom(rows - 1)
{
    m_main = makeGrid(rows, cols);
    m_mainWrapped.assign(rows, false);
}

void Screen::resize(int rows, int cols)
{
    if (rows <= 0 || cols <= 0 || (rows == m_rows && cols == m_cols)) {
        return;
    }
    const int oldCols = m_cols;
    m_rows = rows;
    m_cols = cols;
    if (m_inAlt) {
        // Full-screen apps own their layout; resize the alt grid to the new
        // width. The saved main screen is re-wrapped on leaveAltScreen, so
        // keep it (and its wrapped flags) at the new row count, old width.
        m_alt = resizeGrid(m_alt, rows, cols);
        resizeRowsTo(m_mainSaved, rows, oldCols);
        carryWrappedFlags(m_mainSavedWrapped, rows);
    } else {
        // Keep the grid at the new row count (old width) so reflow() reads
        // valid rows, then re-wrap the main screen + scrollback to the new
        // width.
        resizeRowsTo(m_main, rows, oldCols);
        carryWrappedFlags(m_mainWrapped, rows);
        reflow();
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
    // Explicit positioning starts a new logical line (hard break).
    if (!m_inAlt) {
        m_mainWrapped[m_cursorRow] = false;
    }
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
    if (!m_inAlt) {
        m_mainWrapped[m_cursorRow] = false;
    }
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
        lineFeedImpl(true); // right-margin wrap: the new row is a continuation.
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
    lineFeedImpl(false); // explicit LF: the new row is a hard break.
}

void Screen::lineFeedImpl(bool soft)
{
    if (m_cursorRow >= m_scrollBottom) {
        scrollUpOne(m_scrollTop, m_scrollBottom);
        if (!m_inAlt) {
            m_mainWrapped[m_scrollBottom] = soft;
        }
    } else {
        ++m_cursorRow;
        if (!m_inAlt) {
            m_mainWrapped[m_cursorRow] = soft;
        }
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
    m_scrollbackWrapped.clear();
}

void Screen::enterAltScreen()
{
    if (m_inAlt) {
        return;
    }
    m_mainSaved = m_main;
    m_mainSavedWrapped = m_mainWrapped;
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
    m_mainWrapped = m_mainSavedWrapped;
    m_inAlt = false;
    // The restored main screen was last laid out at the old width; re-wrap
    // it to the current width.
    reflow();
    moveCursor(0, 0);
}

void Screen::fullReset()
{
    auto &g = grid();
    g.assign(m_rows, Line(m_cols, blankCell()));
    if (!m_inAlt) {
        m_mainWrapped.assign(m_rows, false);
    }
    moveCursor(0, 0);
    m_pendingWrap = false;
    resetScrollRegion();
}

void Screen::scrollUpOne(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    auto &g = grid();
    if (top == 0 && !m_inAlt) {
        pushScrollback(g[top], m_mainWrapped[top]);
    }
    for (int r = top; r < bottom; ++r) {
        g[r] = g[r + 1];
    }
    blankLine(bottom);
    if (!m_inAlt) {
        for (int r = top; r < bottom; ++r) {
            m_mainWrapped[r] = m_mainWrapped[r + 1];
        }
        // m_mainWrapped[bottom] is set by the caller (lineFeedImpl / scrollUp).
    }
}

void Screen::scrollDownOne(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    auto &g = grid();
    for (int r = bottom; r > top; --r) {
        g[r] = g[r - 1];
    }
    blankLine(top);
    if (!m_inAlt) {
        for (int r = bottom; r > top; --r) {
            m_mainWrapped[r] = m_mainWrapped[r - 1];
        }
        m_mainWrapped[top] = false; // fresh top row is a hard break.
    }
}

void Screen::blankLine(int row)
{
    grid()[row].assign(m_cols, blankCell());
}

void Screen::pushScrollback(const Line &line, bool wrapped)
{
    m_scrollback.push_back(line);
    m_scrollbackWrapped.push_back(wrapped);
    while (static_cast<int>(m_scrollback.size()) > kMaxScrollback) {
        m_scrollback.pop_front();
        m_scrollbackWrapped.pop_front();
    }
}

void Screen::reflow()
{
    if (m_inAlt) {
        return;
    }
    // 1. Collect physical lines: scrollback (oldest -> newest), then screen
    //    (top -> bottom).
    std::vector<Line> phys;
    std::vector<bool> wrapped;
    phys.reserve(m_scrollback.size() + m_rows);
    wrapped.reserve(m_scrollback.size() + m_rows);
    for (size_t i = 0; i < m_scrollback.size(); ++i) {
        phys.push_back(m_scrollback[i]);
        wrapped.push_back(m_scrollbackWrapped[i]);
    }
    for (int r = 0; r < m_rows; ++r) {
        phys.push_back(m_main[r]);
        wrapped.push_back(m_mainWrapped[r]);
    }
    // 2. Build logical lines: merge soft continuations, then trim the trailing
    //    padding of each logical line. Spaces at a wrap point are content
    //    (they separate words), so only the end of the whole logical line is
    //    padding.
    std::vector<std::vector<Cell>> logical;
    auto trimEnd = [](std::vector<Cell> &line) {
        while (!line.empty() && line.back().ch == ' ') {
            line.pop_back();
        }
    };
    std::vector<Cell> cur;
    bool curValid = false;
    for (size_t i = 0; i < phys.size(); ++i) {
        if (!curValid) {
            cur = phys[i];
            curValid = true;
        } else if (wrapped[i]) {
            cur.insert(cur.end(), phys[i].begin(), phys[i].end());
        } else {
            trimEnd(cur);
            logical.push_back(std::move(cur));
            cur = phys[i];
            curValid = true;
        }
    }
    if (curValid) {
        trimEnd(cur);
        logical.push_back(std::move(cur));
    }
    // 3. Re-wrap each logical line at the new width.
    std::vector<Line> newPhys;
    std::vector<bool> newWrapped;
    for (auto &log : logical) {
        if (log.empty()) {
            newPhys.emplace_back(m_cols, blankCell());
            newWrapped.push_back(false);
            continue;
        }
        bool first = true;
        for (size_t i = 0; i < log.size(); i += static_cast<size_t>(m_cols)) {
            Line line;
            line.reserve(m_cols);
            for (size_t j = i; j < i + static_cast<size_t>(m_cols) && j < log.size(); ++j) {
                line.push_back(log[j]);
            }
            while (line.size() < static_cast<size_t>(m_cols)) {
                line.push_back(blankCell());
            }
            newPhys.push_back(std::move(line));
            newWrapped.push_back(!first);
            first = false;
        }
    }
    // 4. Redistribute: last `rows` lines -> screen, the rest -> scrollback.
    const size_t total = newPhys.size();
    const size_t maxTotal = static_cast<size_t>(m_rows) + kMaxScrollback;
    const size_t keepFrom = total > maxTotal ? total - maxTotal : 0;
    const size_t screenStart = total > static_cast<size_t>(m_rows) ? total - static_cast<size_t>(m_rows) : 0;
    m_scrollback.clear();
    m_scrollbackWrapped.clear();
    for (size_t i = keepFrom; i < screenStart; ++i) {
        m_scrollback.push_back(newPhys[i]);
        m_scrollbackWrapped.push_back(newWrapped[i]);
    }
    for (int r = 0; r < m_rows; ++r) {
        const size_t idx = screenStart + static_cast<size_t>(r);
        if (idx < total) {
            m_main[r] = newPhys[idx];
            m_mainWrapped[r] = newWrapped[idx];
        } else {
            m_main[r] = Line(m_cols, blankCell());
            m_mainWrapped[r] = false;
        }
    }
    // 5. Cursor: bottom row, column clamped.
    m_cursorRow = m_rows - 1;
    m_cursorCol = std::min(m_cursorCol, m_cols - 1);
    m_pendingWrap = false;
}

} // namespace konsoll
