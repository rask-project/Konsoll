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

bool hasFlag(const Cell &cell, unsigned int flag)
{
    return (cell.flags & flag) != 0u;
}

unsigned char withFlag(unsigned char flags, unsigned int flag)
{
    return static_cast<unsigned char>(flags | flag);
}

unsigned char withoutFlag(unsigned char flags, unsigned int flag)
{
    return static_cast<unsigned char>(flags & ~flag);
}

// Right-margin padding left when a wide character wrapped: a spacer cell with
// no wide lead before it. Not content — dropped when lines are re-joined.
bool isWrapPadding(const std::vector<Cell> &line, std::size_t index)
{
    return hasFlag(line[index], CellFlag::WideSpacer) && (index == 0 || !hasFlag(line[index - 1], CellFlag::Wide));
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
    return std::ranges::all_of(line, [](const Cell &cell) { return cell == blankCell(); });
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
    resetTabStops(0);
}

// Default tab stops every 8 columns from `fromCol` on; earlier columns keep
// what HTS/TBC set.
void Screen::resetTabStops(int fromCol)
{
    m_tabStops.resize(static_cast<std::size_t>(m_cols), false);
    std::ranges::for_each(std::views::iota(std::clamp(fromCol, 0, m_cols), m_cols), [this](int col) {
        m_tabStops[static_cast<std::size_t>(col)] = col > 0 && col % 8 == 0;
    });
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
    resetTabStops(oldCols);
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

void Screen::moveCursorAddressed(int row, int col)
{
    if (originMode()) {
        moveCursor(std::clamp(row + m_scrollTop, m_scrollTop, m_scrollBottom), col);
    } else {
        moveCursor(row, col);
    }
}

void Screen::cursorUp(int count)
{
    const int top = m_cursorRow >= m_scrollTop ? m_scrollTop : 0;
    moveCursor(std::max(m_cursorRow - std::max(count, 1), top), m_cursorCol);
}

void Screen::cursorDown(int count)
{
    const int bottom = m_cursorRow <= m_scrollBottom ? m_scrollBottom : m_rows - 1;
    moveCursor(std::min(m_cursorRow + std::max(count, 1), bottom), m_cursorCol);
}

void Screen::saveCursor()
{
    m_savedCursors[m_inAlt ? 1 : 0] = SavedCursor{
        .row = m_cursorRow,
        .col = m_cursorCol,
        .state = (m_pendingWrap ? SavedPendingWrap : 0) | (originMode() ? SavedOriginMode : 0),
    };
}

void Screen::restoreCursor()
{
    applySavedCursor(m_savedCursors[m_inAlt ? 1 : 0]);
    if (!m_inAlt) {
        m_mainWrapped[m_cursorRow] = false;
    }
}

void Screen::applySavedCursor(const SavedCursor &saved)
{
    m_cursorRow = std::clamp(saved.row, 0, m_rows - 1);
    m_cursorCol = std::clamp(saved.col, 0, m_cols - 1);
    m_pendingWrap = (saved.state & SavedPendingWrap) != 0 && autoWrap();
    setOriginMode((saved.state & SavedOriginMode) != 0);
}

void Screen::setOriginMode(bool on)
{
    m_modes = on ? withFlag(m_modes, OriginMode) : withoutFlag(m_modes, OriginMode);
}

void Screen::setAutoWrap(bool on)
{
    m_modes = on ? withFlag(m_modes, AutoWrapMode) : withoutFlag(m_modes, AutoWrapMode);
    if (!on) {
        m_pendingWrap = false;
    }
}

void Screen::setInsertMode(bool on)
{
    m_modes = on ? withFlag(m_modes, InsertMode) : withoutFlag(m_modes, InsertMode);
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

int Screen::previousCharCol() const
{
    int col = m_pendingWrap ? m_cursorCol : m_cursorCol - 1;
    // Step from a wide character's spacer back to its lead.
    if (col > 0 && hasFlag(grid()[m_cursorRow][col], CellFlag::WideSpacer) &&
        hasFlag(grid()[m_cursorRow][col - 1], CellFlag::Wide)) {
        --col;
    }
    return col;
}

std::optional<unsigned int> Screen::previousChar() const
{
    const int col = previousCharCol();
    if (col < 0) {
        return std::nullopt;
    }
    return grid()[m_cursorRow][col].ch;
}

void Screen::replacePreviousChar(unsigned int ch)
{
    const int col = previousCharCol();
    if (col >= 0) {
        grid()[m_cursorRow][col].ch = ch;
    }
}

void Screen::widenPreviousChar()
{
    // A pending wrap means the previous character is in the last column: no
    // room for its second cell.
    const int col = m_cursorCol - 1;
    if (m_pendingWrap || col < 0) {
        return;
    }
    Line &line = grid()[m_cursorRow];
    if (hasFlag(line[col], CellFlag::Wide) || hasFlag(line[col], CellFlag::WideSpacer)) {
        return;
    }
    breakWideAt(m_cursorCol);
    line[col].flags = withFlag(line[col].flags, CellFlag::Wide);
    line[m_cursorCol] = line[col];
    line[m_cursorCol].ch = ' ';
    line[m_cursorCol].flags = withFlag(withoutFlag(line[col].flags, CellFlag::Wide), CellFlag::WideSpacer);
    if (m_cursorCol + 1 >= m_cols) {
        m_pendingWrap = true;
    } else {
        ++m_cursorCol;
    }
}

void Screen::breakWideAt(int col)
{
    if (col < 0 || col >= m_cols) {
        return;
    }
    Line &line = grid()[m_cursorRow];
    if (hasFlag(line[col], CellFlag::WideSpacer) && col > 0 && hasFlag(line[col - 1], CellFlag::Wide)) {
        line[col - 1].ch = ' ';
        line[col - 1].flags = withoutFlag(line[col - 1].flags, CellFlag::Wide);
        line[col].flags = withoutFlag(line[col].flags, CellFlag::WideSpacer);
    }
    if (hasFlag(line[col], CellFlag::Wide) && col + 1 < m_cols) {
        line[col + 1].flags = withoutFlag(line[col + 1].flags, CellFlag::WideSpacer);
        line[col].ch = ' ';
        line[col].flags = withoutFlag(line[col].flags, CellFlag::Wide);
    }
}

void Screen::putChar(unsigned int ch, const Cell &pen)
{
    if (m_pendingWrap) {
        m_cursorCol = 0;
        lineFeedImpl(true); // right-margin wrap: the new row is a continuation.
        m_pendingWrap = false;
    }
    breakWideAt(m_cursorCol);
    if (insertMode()) {
        insertColumns(1); // IRM: shift the rest of the row right first.
    }
    Cell &c = grid()[m_cursorRow][m_cursorCol];
    c = pen;
    c.ch = ch;
    if (m_cursorCol + 1 >= m_cols) {
        // Without DECAWM the cursor stays put and the next character
        // overwrites the last column.
        m_pendingWrap = autoWrap();
    } else {
        ++m_cursorCol;
    }
}

void Screen::putWideChar(unsigned int ch, const Cell &pen)
{
    if (m_cols < 2) {
        putChar(ch, pen);
        return;
    }
    if (m_pendingWrap) {
        m_cursorCol = 0;
        lineFeedImpl(true);
        m_pendingWrap = false;
    }
    if (m_cursorCol + 1 >= m_cols && !autoWrap()) {
        m_cursorCol = m_cols - 2; // no wrap: the pair takes the last two columns
    } else if (m_cursorCol + 1 >= m_cols) {
        // Only the last column is left: pad it and wrap, as xterm does.
        breakWideAt(m_cursorCol);
        grid()[m_cursorRow][m_cursorCol] = Cell{.fg = 0, .bg = pen.bg, .ch = ' ', .flags = CellFlag::WideSpacer};
        m_cursorCol = 0;
        lineFeedImpl(true);
    }
    breakWideAt(m_cursorCol);
    breakWideAt(m_cursorCol + 1);
    if (insertMode()) {
        insertColumns(2);
    }
    Line &line = grid()[m_cursorRow];
    Cell lead = pen;
    lead.ch = ch;
    lead.flags = withFlag(pen.flags, CellFlag::Wide);
    Cell spacer = pen;
    spacer.ch = ' ';
    spacer.flags = withFlag(pen.flags, CellFlag::WideSpacer);
    line[m_cursorCol] = lead;
    line[m_cursorCol + 1] = spacer;
    if (m_cursorCol + 2 >= m_cols) {
        m_cursorCol = m_cols - 1;
        m_pendingWrap = autoWrap();
    } else {
        m_cursorCol += 2;
    }
}

void Screen::lineFeed()
{
    lineFeedImpl(false); // explicit LF: the new row is a hard break.
}

void Screen::lineFeedImpl(bool soft)
{
    // Only the bottom margin scrolls the region; below it (a status line
    // under the scroll region) the cursor moves down to the last row.
    if (m_cursorRow == m_scrollBottom) {
        scrollUpOne(m_scrollTop, m_scrollBottom);
        if (!m_inAlt) {
            m_mainWrapped[m_scrollBottom] = soft;
        }
    } else if (m_cursorRow < m_rows - 1) {
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

void Screen::tab(int count)
{
    std::ranges::for_each(std::views::iota(0, std::max(count, 1)), [this](int) {
        const auto from = m_tabStops.begin() + std::min(m_cursorCol + 1, m_cols);
        const auto stop = std::find(from, m_tabStops.end(), true);
        m_cursorCol = stop == m_tabStops.end() ? m_cols - 1 : static_cast<int>(stop - m_tabStops.begin());
    });
    m_pendingWrap = false;
}

void Screen::backTab(int count)
{
    std::ranges::for_each(std::views::iota(0, std::max(count, 1)), [this](int) {
        const auto before = std::make_reverse_iterator(m_tabStops.begin() + m_cursorCol);
        const auto stop = std::find(before, m_tabStops.rend(), true);
        m_cursorCol = stop == m_tabStops.rend() ? 0 : static_cast<int>(stop.base() - m_tabStops.begin()) - 1;
    });
    m_pendingWrap = false;
}

void Screen::setTabStop()
{
    m_tabStops[static_cast<std::size_t>(m_cursorCol)] = true;
}

void Screen::clearTabStop(int mode)
{
    if (mode == 0) {
        m_tabStops[static_cast<std::size_t>(m_cursorCol)] = false;
    } else if (mode == 3) {
        std::ranges::fill(m_tabStops, false);
    }
}

void Screen::reverseIndex()
{
    // Mirror of LF: only the top margin scrolls the region.
    if (m_cursorRow == m_scrollTop) {
        scrollDownOne(m_scrollTop, m_scrollBottom);
    } else if (m_cursorRow > 0) {
        --m_cursorRow;
    }
}

// Blanks columns [first, last] of `row`, skipping cells whose attrs carry
// any of `protectedMask` (DECSCA / SPA protection).
void Screen::eraseCells(int row, int first, int last, unsigned int protectedMask)
{
    if (first > last) {
        return;
    }
    Line &line = grid()[row];
    const Cell blank = eraseCell();
    std::for_each(line.begin() + first, line.begin() + last + 1, [&blank, &protectedMask](Cell &cell) {
        if ((cell.attrs & protectedMask) == 0u) {
            cell = blank;
        }
    });
}

void Screen::eraseInDisplay(int mode, unsigned int protectedMask)
{
    if (mode == 3) {
        clearScrollback();
        return;
    }
    const auto eraseRows = [this, &protectedMask](int from, int to) {
        std::ranges::for_each(std::views::iota(from, to),
                              [this, &protectedMask](int r) { eraseCells(r, 0, m_cols - 1, protectedMask); });
    };
    if (mode == 0) {
        // Cursor to end of screen.
        breakWideAt(m_cursorCol);
        eraseCells(m_cursorRow, m_cursorCol, m_cols - 1, protectedMask);
        eraseRows(m_cursorRow + 1, m_rows);
    } else if (mode == 1) {
        // Start of screen to cursor.
        breakWideAt(m_cursorCol);
        eraseRows(0, m_cursorRow);
        eraseCells(m_cursorRow, 0, m_cursorCol, protectedMask);
    } else if (mode == 2) {
        eraseRows(0, m_rows);
    }
}

void Screen::eraseInLine(int mode, unsigned int protectedMask)
{
    breakWideAt(m_cursorCol);
    if (mode == 0) {
        eraseCells(m_cursorRow, m_cursorCol, m_cols - 1, protectedMask);
    } else if (mode == 1) {
        eraseCells(m_cursorRow, 0, m_cursorCol, protectedMask);
    } else if (mode == 2) {
        eraseCells(m_cursorRow, 0, m_cols - 1, protectedMask);
    }
}

void Screen::eraseChars(int count, unsigned int protectedMask)
{
    const int last = std::min(m_cursorCol + std::max(count, 1), m_cols) - 1;
    breakWideAt(m_cursorCol);
    breakWideAt(last);
    eraseCells(m_cursorRow, m_cursorCol, last, protectedMask);
}

// SL / SR: shift every line of the scroll region left (positive) or right
// (negative) by `count` columns, blanking the columns shifted in.
void Screen::scrollHorizontally(int count)
{
    const int n = std::min(std::abs(count), m_cols);
    if (n == 0) {
        return;
    }
    const Cell blank = eraseCell();
    std::ranges::for_each(std::views::iota(m_scrollTop, m_scrollBottom + 1), [&](int r) {
        Line &line = grid()[r];
        if (count > 0) {
            std::move(line.begin() + n, line.end(), line.begin());
            std::fill(line.end() - n, line.end(), blank);
        } else {
            std::move_backward(line.begin(), line.end() - n, line.end());
            std::fill(line.begin(), line.begin() + n, blank);
        }
    });
}

void Screen::scrollUp(int count)
{
    const int lines = std::min(std::max(count, 1), m_scrollBottom - m_scrollTop + 1);
    std::ranges::for_each(std::views::iota(0, lines), [this](int) {
        scrollUpOne(m_scrollTop, m_scrollBottom);
        if (!m_inAlt) {
            m_mainWrapped[m_scrollBottom] = false;
        }
    });
}

void Screen::scrollDown(int count)
{
    const int lines = std::min(std::max(count, 1), m_scrollBottom - m_scrollTop + 1);
    std::ranges::for_each(std::views::iota(0, lines), [this](int) { scrollDownOne(m_scrollTop, m_scrollBottom); });
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
    std::fill(g.begin() + m_cursorRow, g.begin() + m_cursorRow + lineCount, Line(m_cols, eraseCell()));

    if (!m_inAlt) {
        const auto wrappedFirst = m_mainWrapped.begin() + m_cursorRow;
        const auto wrappedMiddle = m_mainWrapped.begin() + m_scrollBottom - lineCount + 1;
        const auto wrappedLast = m_mainWrapped.begin() + m_scrollBottom + 1;
        std::move_backward(wrappedFirst, wrappedMiddle, wrappedLast);
        std::fill(m_mainWrapped.begin() + m_cursorRow, m_mainWrapped.begin() + m_cursorRow + lineCount, false);
    }
    m_cursorCol = 0; // IL/DL return the cursor to the left margin.
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
    std::fill(g.begin() + m_scrollBottom - lineCount + 1, g.begin() + m_scrollBottom + 1, Line(m_cols, eraseCell()));

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
    m_cursorCol = 0;
    m_pendingWrap = false;
}

void Screen::setScrollRegion(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    // 1-based inclusive; clamp to the grid. A region of fewer than two
    // lines is invalid and ignored, as in xterm.
    const int t = std::clamp(top - 1, 0, m_rows - 1);
    const int b = std::clamp(bottom - 1, 0, m_rows - 1);
    if (t >= b) {
        return;
    }
    m_scrollTop = t;
    m_scrollBottom = b;
    moveCursorAddressed(0, 0); // DECSTBM homes the cursor (origin-relative).
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
    // Never more than the cells from the cursor to the right margin.
    const int n = std::min(count, m_cols - m_cursorCol);
    breakWideAt(m_cursorCol);
    if (m_cursorCol + n < m_cols) {
        breakWideAt(m_cursorCol + n);
    }
    Line &row = grid()[m_cursorRow];
    std::move(row.begin() + m_cursorCol + n, row.end(), row.begin() + m_cursorCol);
    std::fill(row.end() - n, row.end(), eraseCell());
    m_pendingWrap = false;
}

void Screen::insertColumns(int count)
{
    if (count <= 0) {
        return;
    }
    const int n = std::min(count, m_cols - m_cursorCol);
    breakWideAt(m_cursorCol);
    if (m_cols - n > m_cursorCol) {
        breakWideAt(m_cols - n); // the pair pushed across the right margin
    }
    Line &row = grid()[m_cursorRow];
    std::move_backward(row.begin() + m_cursorCol, row.end() - n, row.end());
    std::fill(row.begin() + m_cursorCol, row.begin() + m_cursorCol + n, eraseCell());
    m_pendingWrap = false;
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
    m_droppedLines += static_cast<long long>(m_scrollback.size());
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
    // The alternate screen has no scrollback: show its bottom. The main
    // scrollback is kept for when the full-screen app exits.
    m_scrollOffset = 0;
    m_pendingWrap = false;
}

void Screen::leaveAltScreen(bool restoreCursor)
{
    if (!m_inAlt) {
        return;
    }
    m_main = m_mainSaved;
    m_mainWrapped = m_mainSavedWrapped;
    m_inAlt = false;
    if (restoreCursor) {
        applySavedCursor(m_savedCursors[0]);
    }
    // The restored main screen was last laid out at the old width; re-wrap
    // it to the current width (keeping the cursor on its content).
    reflow();
    m_pendingWrap = false;
}

void Screen::fullReset()
{
    leaveAltScreen(false);
    m_main.assign(m_rows, Line(m_cols, blankCell()));
    m_mainWrapped.assign(m_rows, false);
    m_eraseBg = 0;
    m_modes = AutoWrapMode;
    m_cursorVisible = true;
    m_savedCursors = {};
    resetScrollRegion();
    std::ranges::fill(m_tabStops, false);
    resetTabStops(0);
    moveCursor(0, 0);
}

void Screen::softReset()
{
    m_modes = AutoWrapMode;
    m_cursorVisible = true;
    m_pendingWrap = false;
    m_savedCursors[m_inAlt ? 1 : 0] = {};
    resetScrollRegion();
}

void Screen::alignmentTest()
{
    resetScrollRegion();
    setOriginMode(false);
    Cell filled = blankCell();
    filled.ch = 'E';
    grid().assign(m_rows, Line(m_cols, filled));
    if (!m_inAlt) {
        m_mainWrapped.assign(m_rows, false);
    }
    moveCursor(0, 0);
}

void Screen::scrollUpOne(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    auto &g = grid();
    if (top == 0 && !m_inAlt) {
        pushScrollback(g[top], m_mainWrapped[top]);
    } else if (top == 0) {
        ++m_altScrolled; // images on the alternate screen scroll with it
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
    grid()[row].assign(m_cols, eraseCell());
}

Cell Screen::eraseCell() const
{
    Cell cell = blankCell();
    cell.bg = m_eraseBg;
    return cell;
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
        ++m_droppedLines;
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
        ++m_droppedLines;
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
        // A wide character's spacer is part of it, not trailing padding.
        while (!line.empty() && line.back().ch == ' ' &&
               (!hasFlag(line.back(), CellFlag::WideSpacer) || isWrapPadding(line, line.size() - 1))) {
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
            if (!cur.empty() && isWrapPadding(cur, cur.size() - 1)) {
                cur.pop_back();
            }
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

// 3. Re-wrap each logical line at the new width. A wide character never
//    straddles two rows: when its lead would land in the last column it moves
//    to the next row and the gap becomes wrap padding.
Screen::ReflowedScreen
Screen::rewrapLogicalLines(const std::vector<std::vector<Cell>> &logical, size_t cursorLogical, size_t cursorLogicalCol,
                           size_t &cursorNewPhys, size_t &cursorNewCol) const
{
    ReflowedScreen rewrapped;
    const auto cols = static_cast<size_t>(m_cols);
    std::vector<size_t> chunkStarts;
    for (size_t logicalIndex = 0; logicalIndex < logical.size(); ++logicalIndex) {
        const size_t logicalStart = rewrapped.lines.size();
        chunkStarts.clear();
        wrapLogicalLine(logical[logicalIndex], rewrapped, chunkStarts);
        if (logicalIndex == cursorLogical) {
            // Last chunk starting at or before the cursor column; columns past
            // it (cursor beyond the trimmed text) spill onto following rows.
            const auto chunk = std::ranges::upper_bound(chunkStarts, cursorLogicalCol) - 1;
            const size_t offset = cursorLogicalCol - *chunk;
            cursorNewPhys = logicalStart + static_cast<size_t>(std::distance(chunkStarts.begin(), chunk)) + (offset / cols);
            cursorNewCol = offset % cols;
        }
    }
    return rewrapped;
}

void Screen::wrapLogicalLine(const std::vector<Cell> &log, ReflowedScreen &out, std::vector<size_t> &chunkStarts) const
{
    const auto cols = static_cast<size_t>(m_cols);
    size_t start = 0;
    while (true) {
        chunkStarts.push_back(start);
        size_t take = std::min(cols, log.size() - start);
        const bool more = start + take < log.size();
        if (more && take > 1 && hasFlag(log[start + take - 1], CellFlag::Wide)) {
            --take;
        }
        Line line(log.begin() + static_cast<std::ptrdiff_t>(start),
                  log.begin() + static_cast<std::ptrdiff_t>(start + take));
        if (more && line.size() < cols) {
            line.push_back(Cell{.fg = 0, .bg = 0, .ch = ' ', .flags = CellFlag::WideSpacer});
        }
        line.resize(cols, blankCell());
        out.lines.push_back(std::move(line));
        out.wrapped.push_back(start != 0);
        start += take;
        if (!more) {
            break;
        }
    }
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
