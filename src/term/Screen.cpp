#include "Screen.h"

#include <algorithm>
#include <ranges>

namespace konsoll
{

namespace
{
Cell blankCell()
{
    return Cell{.fg = 0, .bg = 0, .ch = ' ', .flags = CellFlag::None};
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

bool isBlankLine(const Line &line)
{
    return std::ranges::all_of(line, [](const Cell &cell) {
        return cell.ch == ' ' && cell.fg == 0u && cell.bg == 0u && cell.flags == CellFlag::None;
    });
}

// Add hard-break flags for newly allocated rows without dropping existing
// rows. Reflow needs the complete old screen to preserve its contents.
void ensureWrappedFlags(std::vector<bool> &flags, size_t rows)
{
    flags.resize(std::max(flags.size(), rows), false);
}

// Ensure at least `rows` physical lines exist before reflow. Existing lines
// must remain available, especially when the window gets shorter.
void ensureRows(std::vector<Line> &grid, int rows, int width)
{
    while (static_cast<int>(grid.size()) < rows) {
        grid.emplace_back(width, blankCell());
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
        ensureRows(m_mainSaved, rows, oldCols);
        ensureWrappedFlags(m_mainSavedWrapped, m_mainSaved.size());
    } else {
        if (std::ranges::all_of(m_scrollback, isBlankLine)) {
            m_scrollback.clear();
            m_scrollbackWrapped.clear();
            m_scrollOffset = 0;
        }
        const bool emptyScreen = std::ranges::all_of(m_scrollback, isBlankLine) &&
                                 std::ranges::all_of(m_main, isBlankLine);
        if (emptyScreen) {
            // A smaller pane must not turn unused blank screen rows into
            // scrollback; that makes an idle terminal appear scrollable.
            m_scrollback.clear();
            m_scrollbackWrapped.clear();
            m_scrollOffset = 0;
            m_main = makeGrid(rows, cols);
            m_mainWrapped.assign(rows, false);
        } else {
            // Keep every old physical row until reflow has moved any rows that
            // no longer fit into scrollback. Truncating first loses the bottom
            // of the terminal whenever a pane becomes shorter.
            ensureRows(m_main, rows, oldCols);
            ensureWrappedFlags(m_mainWrapped, m_main.size());
            reflow();
        }
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

void Screen::putChar(unsigned int ch, unsigned int fg, unsigned int bg, unsigned char flags) // NOLINT: fg/bg are
                                                                                             // inherently swappable
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

void Screen::insertLines(int count)
{
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom) {
        return;
    }

    const int lineCount = std::min(count, m_scrollBottom - m_cursorRow + 1);
    if (lineCount <= 0) {
        return;
    }

    auto &g = grid();
    const auto first = g.begin() + m_cursorRow;
    const auto middle = g.begin() + m_scrollBottom - lineCount + 1;
    const auto last = g.begin() + m_scrollBottom + 1;
    std::move_backward(first, middle, last);
    std::fill(g.begin() + m_cursorRow, g.begin() + m_cursorRow + lineCount, Line(m_cols, blankCell()));

    if (!m_inAlt) {
        const auto wrappedFirst = m_mainWrapped.begin() + m_cursorRow;
        const auto wrappedMiddle = m_mainWrapped.begin() + m_scrollBottom - lineCount + 1;
        const auto wrappedLast = m_mainWrapped.begin() + m_scrollBottom + 1;
        std::move_backward(wrappedFirst, wrappedMiddle, wrappedLast);
        std::fill(m_mainWrapped.begin() + m_cursorRow, m_mainWrapped.begin() + m_cursorRow + lineCount, false);
    }
    m_pendingWrap = false;
}

void Screen::deleteLines(int count)
{
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom) {
        return;
    }

    const int lineCount = std::min(count, m_scrollBottom - m_cursorRow + 1);
    if (lineCount <= 0) {
        return;
    }

    auto &g = grid();
    std::move(g.begin() + m_cursorRow + lineCount, g.begin() + m_scrollBottom + 1, g.begin() + m_cursorRow);
    std::fill(g.begin() + m_scrollBottom - lineCount + 1, g.begin() + m_scrollBottom + 1, Line(m_cols, blankCell()));

    if (!m_inAlt) {
        std::move(
            m_mainWrapped.begin() + m_cursorRow + lineCount,
            m_mainWrapped.begin() + m_scrollBottom + 1,
            m_mainWrapped.begin() + m_cursorRow
        );
        std::fill(
            m_mainWrapped.begin() + m_scrollBottom - lineCount + 1,
            m_mainWrapped.begin() + m_scrollBottom + 1,
            false
        );
    }
    m_pendingWrap = false;
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
    m_scrollOffset = 0;
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
    // A new line lands at the bottom: if the view is scrolled up, keep it on
    // the same absolute line (the offset grows with the bottom).
    if (m_scrollOffset > 0) {
        ++m_scrollOffset;
    }

    // Cap only when a limit is set (0 = unlimited).
    while (m_maxScrollback > 0 && static_cast<int>(m_scrollback.size()) > m_maxScrollback) {
        m_scrollback.pop_front();
        m_scrollbackWrapped.pop_front();
        // Popping the oldest line shifts the view down by one; keep it on the
        // same absolute line.
        if (m_scrollOffset > 0) {
            --m_scrollOffset;
        }
    }
}

void Screen::setMaxScrollback(int max)
{
    m_maxScrollback = std::max(0, max);
    // Trim if the new limit is smaller than the current scrollback.
    while (m_maxScrollback > 0 && static_cast<int>(m_scrollback.size()) > m_maxScrollback) {
        m_scrollback.pop_front();
        m_scrollbackWrapped.pop_front();
        if (m_scrollOffset > 0) {
            --m_scrollOffset;
        }
    }
}

int Screen::totalLines() const
{
    if (m_inAlt) {
        return m_rows;
    }
    return static_cast<int>(m_scrollback.size()) + m_rows;
}

int Screen::maxScrollOffset() const
{
    if (m_inAlt) {
        return 0;
    }
    return static_cast<int>(m_scrollback.size());
}

void Screen::scrollBy(int delta)
{
    m_scrollOffset = std::clamp(m_scrollOffset + delta, 0, maxScrollOffset());
}

void Screen::scrollTo(int offset)
{
    m_scrollOffset = std::clamp(offset, 0, maxScrollOffset());
}

void Screen::scrollToBottom()
{
    m_scrollOffset = 0;
}

const Line &Screen::lineAt(int index) const
{
    static const Line kEmpty;
    if (index < 0 || index >= totalLines()) {
        return kEmpty;
    }
    if (m_inAlt) {
        return m_alt[index];
    }
    if (index < static_cast<int>(m_scrollback.size())) {
        auto it = m_scrollback.begin();
        std::advance(it, index);
        return *it;
    }
    return m_main[index - static_cast<int>(m_scrollback.size())];
}

bool Screen::isWrappedContinuation(int index) const
{
    if (m_inAlt || index <= 0 || index >= totalLines()) {
        return false;
    }
    const int scrollbackLines = static_cast<int>(m_scrollback.size());
    if (index < scrollbackLines) {
        return m_scrollbackWrapped[index];
    }
    return m_mainWrapped[index - scrollbackLines];
}

void Screen::reflow()
{
    if (m_inAlt) {
        return;
    }
    std::vector<Line> phys;
    std::vector<bool> wrapped;
    collectPhysicalLines(phys, wrapped);
    const size_t cursorPhys = m_scrollback.size() + static_cast<size_t>(m_cursorRow);
    size_t cursorLogical = 0;
    size_t cursorLogicalCol = 0;
    const std::vector<std::vector<Cell>> logical =
        buildLogicalLines(phys, wrapped, cursorPhys, cursorLogical, cursorLogicalCol);
    size_t cursorNewPhys = 0;
    size_t cursorNewCol = 0;
    const ReflowedScreen rewrapped =
        rewrapLogicalLines(logical, cursorLogical, cursorLogicalCol, cursorNewPhys, cursorNewCol);
    redistribute(rewrapped, cursorNewPhys, cursorNewCol);
}

// 1. Collect physical lines: scrollback (oldest -> newest), then screen
//    (top -> bottom).
void Screen::collectPhysicalLines(std::vector<Line> &phys, std::vector<bool> &wrapped) const
{
    size_t screenRows = std::min(m_main.size(), static_cast<size_t>(m_cursorRow + 1));
    for (size_t row = screenRows; row < m_main.size(); ++row) {
        if (!isBlankLine(m_main[row])) {
            screenRows = row + 1;
        }
    }
    phys.reserve(m_scrollback.size() + screenRows);
    wrapped.reserve(m_scrollback.size() + screenRows);
    for (size_t i = 0; i < m_scrollback.size(); ++i) {
        phys.push_back(m_scrollback[i]);
        wrapped.push_back(m_scrollbackWrapped[i]);
    }
    for (size_t r = 0; r < screenRows; ++r) {
        phys.push_back(m_main[r]);
        wrapped.push_back(m_mainWrapped[r]);
    }
}

// 2. Build logical lines: merge soft continuations, then trim the trailing
//    padding of each logical line. Spaces at a wrap point are content
//    (they separate words), so only the end of the whole logical line is
//    padding.
std::vector<std::vector<Cell>>
Screen::buildLogicalLines(const std::vector<Line> &phys, const std::vector<bool> &wrapped, size_t cursorPhys,
                           size_t &cursorLogical, size_t &cursorLogicalCol) const
{
    auto const trimEnd = [](std::vector<Cell> &line) {
        while (!line.empty() && line.back().ch == ' ') {
            line.pop_back();
        }
    };
    std::vector<std::vector<Cell>> logical;
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
        if (i == cursorPhys) {
            cursorLogical = logical.size();
            cursorLogicalCol = (wrapped[i] ? cur.size() - phys[i].size() : 0) + static_cast<size_t>(m_cursorCol);
        }
    }
    if (curValid) {
        trimEnd(cur);
        logical.push_back(std::move(cur));
    }
    return logical;
}

// 3. Re-wrap each logical line at the new width.
Screen::ReflowedScreen
Screen::rewrapLogicalLines(const std::vector<std::vector<Cell>> &logical, size_t cursorLogical, size_t cursorLogicalCol,
                           size_t &cursorNewPhys, size_t &cursorNewCol) const
{
    ReflowedScreen rewrapped;
    for (size_t logicalIndex = 0; logicalIndex < logical.size(); ++logicalIndex) {
        auto &log = logical[logicalIndex];
        const size_t logicalStart = rewrapped.lines.size();
        if (log.empty()) {
            rewrapped.lines.emplace_back(m_cols, blankCell());
            rewrapped.wrapped.push_back(false);
        } else {
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
                rewrapped.lines.push_back(std::move(line));
                rewrapped.wrapped.push_back(!first);
                first = false;
            }
        }
        if (logicalIndex == cursorLogical) {
            cursorNewPhys = logicalStart + (cursorLogicalCol / static_cast<size_t>(m_cols));
            cursorNewCol = cursorLogicalCol % static_cast<size_t>(m_cols);
        }
    }
    return rewrapped;
}

// 4. Redistribute: last `rows` lines -> screen, the rest -> scrollback.
// 5. Keep the cursor attached to its logical line and column. This keeps
//    it in place when the window grows instead of anchoring it to the
//    bottom row of the newly larger screen.
void Screen::redistribute(const ReflowedScreen &rewrapped, size_t cursorNewPhys, size_t cursorNewCol)
{
    const size_t total = rewrapped.lines.size();
    // Cap only when a limit is set (0 = unlimited).
    const size_t maxTotal =
        m_maxScrollback > 0 ? static_cast<size_t>(m_rows) + static_cast<size_t>(m_maxScrollback) : total;
    const size_t keepFrom = total > maxTotal ? total - maxTotal : 0;
    const size_t screenStart = total > static_cast<size_t>(m_rows) ? total - static_cast<size_t>(m_rows) : 0;
    m_main.resize(static_cast<size_t>(m_rows));
    m_mainWrapped.resize(static_cast<size_t>(m_rows), false);
    m_scrollback.clear();
    m_scrollbackWrapped.clear();
    for (size_t i = keepFrom; i < screenStart; ++i) {
        m_scrollback.push_back(rewrapped.lines[i]);
        m_scrollbackWrapped.push_back(rewrapped.wrapped[i]);
    }
    for (int r = 0; r < m_rows; ++r) {
        const size_t idx = screenStart + static_cast<size_t>(r);
        if (idx < total) {
            m_main[r] = rewrapped.lines[idx];
            m_mainWrapped[r] = rewrapped.wrapped[idx];
        } else {
            m_main[r] = Line(m_cols, blankCell());
            m_mainWrapped[r] = false;
        }
    }
    m_cursorRow = std::clamp(static_cast<int>(cursorNewPhys) - static_cast<int>(screenStart), 0, m_rows - 1);
    m_cursorCol = std::min(static_cast<int>(cursorNewCol), m_cols - 1);
    m_pendingWrap = false;
    m_scrollOffset = 0;
}

} // namespace konsoll
