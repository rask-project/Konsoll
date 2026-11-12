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

Screen::Screen(int rows, int cols) : m_rows(rows), m_cols(cols), m_scrollBottom(rows - 1), m_marginRight(cols - 1)
{
    m_main = makeGrid(rows, cols);
    m_mainWrapped.assign(rows, false);
    m_lineSizes.assign(static_cast<std::size_t>(rows), SingleLine);
    resetTabStops(0);
}

int Screen::lineSize(int index) const
{
    const int row = index - (m_inAlt ? 0 : static_cast<int>(m_scrollback.size()));
    return row >= 0 && row < static_cast<int>(m_lineSizes.size()) ? m_lineSizes[static_cast<std::size_t>(row)]
                                                                  : SingleLine;
}

void Screen::setLineSize(int size)
{
    m_lineSizes[static_cast<std::size_t>(m_cursorRow)] = static_cast<unsigned char>(std::clamp(size, 0, 3));
    // The right half of a double-size line is not shown; keep the cursor in
    // the visible half.
    m_cursorCol = std::min(m_cursorCol, rowColumns(m_cursorRow) - 1);
    m_pendingWrap = false;
}

int Screen::rowColumns(int row) const
{
    const bool doubled = row >= 0 && row < static_cast<int>(m_lineSizes.size()) &&
                         m_lineSizes[static_cast<std::size_t>(row)] != SingleLine;
    return doubled ? std::max(m_cols / 2, 1) : m_cols;
}

// Keep line sizes with their rows when [top, bottom] scrolls up (count > 0)
// or down; rows shifted in are single.
void Screen::rotateLineSizes(int top, int bottom, int count)
{
    const int n = std::min(std::abs(count), bottom - top + 1);
    if (n <= 0) {
        return;
    }
    const auto first = m_lineSizes.begin() + top;
    const auto last = m_lineSizes.begin() + bottom + 1;
    if (count > 0) {
        std::move(first + n, last, first);
        std::fill(last - n, last, SingleLine);
    } else {
        std::move_backward(first, last - n, last);
        std::fill(first, first + n, SingleLine);
    }
}

void Screen::resetLineSizes()
{
    m_lineSizes.assign(static_cast<std::size_t>(m_rows), SingleLine);
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
        if (m_scrollback.contentLines() == 0 && !m_scrollback.empty()) {
            m_scrollback.clear();
            m_scrollOffset = 0;
        }
        const bool emptyScreen = m_scrollback.empty() && std::ranges::all_of(m_main, isBlankLine);
        if (emptyScreen) {
            // A smaller pane must not turn unused blank screen rows into
            // scrollback; that makes an idle terminal appear scrollable.
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
    m_marginLeft = 0;
    m_marginRight = cols - 1;
    m_pendingWrap = false;
    resetTabStops(oldCols);
    // The main screen's reflow placed the line sizes; the alternate screen
    // and the saved main screen keep theirs by row.
    m_lineSizes.resize(static_cast<std::size_t>(rows), SingleLine);
    m_mainSavedLineSizes.resize(static_cast<std::size_t>(rows), SingleLine);
}

void Screen::moveCursor(int row, int col)
{
    m_cursorRow = std::clamp(row, 0, m_rows - 1);
    m_cursorCol = std::clamp(col, 0, rowColumns(m_cursorRow) - 1);
    m_pendingWrap = false;
    // Explicit positioning starts a new logical line (hard break).
    if (!m_inAlt) {
        m_mainWrapped[m_cursorRow] = false;
    }
}

void Screen::moveCursorAddressed(int row, int col)
{
    if (originMode()) {
        moveCursor(
            std::clamp(row + m_scrollTop, m_scrollTop, m_scrollBottom),
            std::clamp(col + leftMargin(), leftMargin(), rightMargin())
        );
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

void Screen::setGraphemeClustering(bool on)
{
    m_modes = on ? withFlag(m_modes, GraphemeMode) : withoutFlag(m_modes, GraphemeMode);
}

bool Screen::previousCharIsWide() const
{
    const int col = previousCharCol();
    return col >= 0 && hasFlag(grid()[m_cursorRow][col], CellFlag::Wide);
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

// The column after which a character wraps: the right margin when the
// cursor is inside the left/right margins, else the last column.
int Screen::wrapColumn() const
{
    const int last = m_cursorCol <= rightMargin() ? rightMargin() : m_cols - 1;
    return std::min(last, rowColumns(m_cursorRow) - 1);
}

void Screen::wrapToNextLine()
{
    // Wrapping from the right margin continues at the left margin.
    m_cursorCol = m_cursorCol == rightMargin() ? leftMargin() : 0;
    lineFeedImpl(true); // right-margin wrap: the new row is a continuation.
    m_pendingWrap = false;
}

void Screen::putChar(unsigned int ch, const Cell &pen)
{
    if (m_pendingWrap) {
        wrapToNextLine();
    }
    breakWideAt(m_cursorCol);
    if (insertMode()) {
        insertColumns(1); // IRM: shift the rest of the row right first.
    }
    Cell &c = grid()[m_cursorRow][m_cursorCol];
    c = pen;
    c.ch = ch;
    if (m_cursorCol >= wrapColumn()) {
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
        wrapToNextLine();
    }
    const int lastColumn = wrapColumn();
    if (m_cursorCol + 1 > lastColumn && !autoWrap()) {
        m_cursorCol = lastColumn - 1; // no wrap: the pair takes the last two columns
    } else if (m_cursorCol + 1 > lastColumn) {
        // Only the last column is left: pad it and wrap, as xterm does.
        breakWideAt(m_cursorCol);
        grid()[m_cursorRow][m_cursorCol] = Cell{.fg = 0, .bg = pen.bg, .ch = ' ', .flags = CellFlag::WideSpacer};
        wrapToNextLine();
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
    if (m_cursorCol + 2 > wrapColumn()) {
        m_cursorCol = wrapColumn();
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
    // under the scroll region) the cursor moves down to the last row. With
    // left/right margins only a cursor inside them scrolls.
    if (m_cursorRow == m_scrollBottom) {
        if (hasSideMargins() && !insideMargins()) {
            return;
        }
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
    m_cursorCol = m_cursorCol >= leftMargin() ? leftMargin() : 0;
    m_pendingWrap = false;
}

void Screen::backspace()
{
    const bool atLeft = m_cursorCol == 0 || m_cursorCol == leftMargin();
    if (!atLeft) {
        --m_cursorCol;
    } else if (extendedReverseWrap() || reverseWrap()) {
        // The row above wrapped into this one (the alternate screen does not
        // record wraps, so any line counts there).
        const bool wrapped = m_inAlt || (m_cursorRow > 0 && m_mainWrapped[static_cast<std::size_t>(m_cursorRow)]);
        if (m_cursorRow > m_scrollTop && (wrapped || extendedReverseWrap())) {
            --m_cursorRow;
            m_cursorCol = rightMargin();
        } else if (m_cursorRow == m_scrollTop && extendedReverseWrap()) {
            m_cursorRow = m_scrollBottom; // ?1045 wraps from the top to the bottom margin
            m_cursorCol = rightMargin();
        }
    }
    m_pendingWrap = false;
}

void Screen::setReverseWrap(bool on)
{
    m_modes = on ? withFlag(m_modes, ReverseWrapMode) : withoutFlag(m_modes, ReverseWrapMode);
}

void Screen::setExtendedReverseWrap(bool on)
{
    m_modes = on ? withFlag(m_modes, ExtendedReverseWrapMode) : withoutFlag(m_modes, ExtendedReverseWrapMode);
}

void Screen::tab(int count)
{
    std::ranges::for_each(std::views::iota(0, std::max(count, 1)), [this](int) {
        // Tabs stop at the right margin when the cursor is inside it.
        const int limit = wrapColumn();
        const auto from = m_tabStops.begin() + std::min(m_cursorCol + 1, m_cols);
        const auto stop = std::find(from, m_tabStops.end(), true);
        const int next = stop == m_tabStops.end() ? m_cols - 1 : static_cast<int>(stop - m_tabStops.begin());
        m_cursorCol = std::min(next, limit);
    });
    m_pendingWrap = false;
}

void Screen::backTab(int count)
{
    std::ranges::for_each(std::views::iota(0, std::max(count, 1)), [this](int) {
        const int limit = m_cursorCol >= leftMargin() ? leftMargin() : 0;
        const auto before = std::make_reverse_iterator(m_tabStops.begin() + m_cursorCol);
        const auto stop = std::find(before, m_tabStops.rend(), true);
        const int previous = stop == m_tabStops.rend() ? 0 : static_cast<int>(stop.base() - m_tabStops.begin()) - 1;
        m_cursorCol = std::max(previous, limit);
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
        if (hasSideMargins() && !insideMargins()) {
            return;
        }
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
        std::ranges::for_each(std::views::iota(from, to), [this, &protectedMask](int r) {
            eraseCells(r, 0, m_cols - 1, protectedMask);
        });
    };
    // Lines erased completely become single width (DEC).
    const auto singleRows = [this](int from, int to) {
        std::fill(m_lineSizes.begin() + from, m_lineSizes.begin() + to, SingleLine);
    };
    if (mode == 0) {
        // Cursor to end of screen.
        breakWideAt(m_cursorCol);
        eraseCells(m_cursorRow, m_cursorCol, m_cols - 1, protectedMask);
        eraseRows(m_cursorRow + 1, m_rows);
        singleRows(m_cursorRow + (m_cursorCol == 0 ? 0 : 1), m_rows);
    } else if (mode == 1) {
        // Start of screen to cursor.
        breakWideAt(m_cursorCol);
        eraseRows(0, m_cursorRow);
        eraseCells(m_cursorRow, 0, m_cursorCol, protectedMask);
        singleRows(0, m_cursorRow);
    } else if (mode == 2) {
        eraseRows(0, m_rows);
        singleRows(0, m_rows);
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
    shiftColumns(m_scrollTop, m_scrollBottom, leftMargin(), rightMargin(), count);
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
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom || !insideMargins()) {
        return;
    }
    if (hasSideMargins()) {
        shiftRows(m_cursorRow, m_scrollBottom, leftMargin(), rightMargin(), -std::max(count, 1));
        m_cursorCol = leftMargin();
        m_pendingWrap = false;
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
    rotateLineSizes(m_cursorRow, m_scrollBottom, -lineCount);

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
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom || !insideMargins()) {
        return;
    }
    if (hasSideMargins()) {
        shiftRows(m_cursorRow, m_scrollBottom, leftMargin(), rightMargin(), std::max(count, 1));
        m_cursorCol = leftMargin();
        m_pendingWrap = false;
        return;
    }

    const int lineCount = std::min(count, m_scrollBottom - m_cursorRow + 1);
    if (lineCount <= 0) {
        return;
    }

    auto &g = grid();
    std::move(g.begin() + m_cursorRow + lineCount, g.begin() + m_scrollBottom + 1, g.begin() + m_cursorRow);
    std::fill(g.begin() + m_scrollBottom - lineCount + 1, g.begin() + m_scrollBottom + 1, Line(m_cols, eraseCell()));
    rotateLineSizes(m_cursorRow, m_scrollBottom, lineCount);

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
    if (count <= 0 || (hasSideMargins() && !insideMargins())) {
        return;
    }
    // Never more than the cells from the cursor to the right margin.
    const int end = wrapColumn() + 1;
    const int n = std::min(count, end - m_cursorCol);
    breakWideAt(m_cursorCol);
    if (m_cursorCol + n < end) {
        breakWideAt(m_cursorCol + n);
    }
    shiftColumns(m_cursorRow, m_cursorRow, m_cursorCol, end - 1, n);
    m_pendingWrap = false;
}

void Screen::insertColumns(int count)
{
    if (count <= 0 || (hasSideMargins() && !insideMargins())) {
        return;
    }
    const int end = wrapColumn() + 1;
    const int n = std::min(count, end - m_cursorCol);
    breakWideAt(m_cursorCol);
    if (end - n > m_cursorCol) {
        breakWideAt(end - n); // the pair pushed across the right margin
    }
    shiftColumns(m_cursorRow, m_cursorRow, m_cursorCol, end - 1, -n);
    m_pendingWrap = false;
}

Line Screen::scrollbackLine(int index) const
{
    Line line;
    if (index >= 0) {
        m_scrollback.line(static_cast<std::size_t>(index), line);
    }
    return line;
}

void Screen::clearScrollback()
{
    m_droppedLines += static_cast<long long>(m_scrollback.size());
    m_scrollback.clear();
    m_scrollOffset = 0;
}

void Screen::enterAltScreen()
{
    if (m_inAlt) {
        return;
    }
    m_mainSaved = m_main;
    m_mainSavedWrapped = m_mainWrapped;
    m_mainSavedLineSizes = m_lineSizes;
    resetLineSizes();
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
    m_lineSizes = m_mainSavedLineSizes;
    m_lineSizes.resize(static_cast<std::size_t>(m_rows), SingleLine);
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
    resetLineSizes();
    m_eraseBg = 0;
    m_modes = AutoWrapMode;
    m_cursorVisible = true;
    m_savedCursors = {};
    m_marginLeft = 0;
    m_marginRight = m_cols - 1;
    resetScrollRegion();
    std::ranges::fill(m_tabStops, false);
    resetTabStops(0);
    moveCursor(0, 0);
}

void Screen::softReset()
{
    m_modes = AutoWrapMode;
    m_marginLeft = 0;
    m_marginRight = m_cols - 1;
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
    resetLineSizes();
    if (!m_inAlt) {
        m_mainWrapped.assign(m_rows, false);
    }
    moveCursor(0, 0);
}

void Screen::scrollUpOne(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    if (hasSideMargins()) {
        // A rectangle scrolls; nothing reaches the history.
        shiftRows(top, bottom, leftMargin(), rightMargin(), 1);
        return;
    }
    auto &g = grid();
    if (top == 0 && !m_inAlt) {
        pushScrollback(g[top], m_mainWrapped[top]);
    } else if (top == 0) {
        ++m_altScrolled; // images on the alternate screen scroll with it
    }
    // Rotate, not copy: the rows swap their buffers and the old top row is
    // reused (blanked) as the new bottom one.
    std::rotate(g.begin() + top, g.begin() + top + 1, g.begin() + bottom + 1);
    blankLine(bottom);
    rotateLineSizes(top, bottom, 1);
    if (!m_inAlt) {
        std::rotate(m_mainWrapped.begin() + top, m_mainWrapped.begin() + top + 1, m_mainWrapped.begin() + bottom + 1);
        // m_mainWrapped[bottom] is set by the caller (lineFeedImpl / scrollUp).
    }
}

void Screen::scrollDownOne(int top, int bottom) // NOLINT: top/bottom are inherently swappable
{
    if (hasSideMargins()) {
        shiftRows(top, bottom, leftMargin(), rightMargin(), -1);
        return;
    }
    auto &g = grid();
    std::rotate(g.begin() + top, g.begin() + bottom, g.begin() + bottom + 1);
    blankLine(top);
    rotateLineSizes(top, bottom, -1);
    if (!m_inAlt) {
        std::rotate(m_mainWrapped.begin() + top, m_mainWrapped.begin() + bottom, m_mainWrapped.begin() + bottom + 1);
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
    m_scrollback.pushBack(line, wrapped);
    // A new line lands at the bottom: if the view is scrolled up, keep it on
    // the same absolute line (the offset grows with the bottom).
    if (m_scrollOffset > 0) {
        ++m_scrollOffset;
    }

    // Cap only when a limit is set (0 = unlimited).
    while (m_maxScrollback > 0 && static_cast<int>(m_scrollback.size()) > m_maxScrollback) {
        m_scrollback.popFront();
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
        m_scrollback.popFront();
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

Line Screen::lineAt(int index) const
{
    Line line;
    copyLine(index, line);
    return line;
}

void Screen::copyLine(int index, Line &out) const
{
    if (index < 0 || index >= totalLines()) {
        out.clear();
        return;
    }
    if (m_inAlt) {
        out = m_alt[static_cast<std::size_t>(index)];
        return;
    }
    const int history = static_cast<int>(m_scrollback.size());
    if (index < history) {
        m_scrollback.line(static_cast<std::size_t>(index), out);
        return;
    }
    out = m_main[static_cast<std::size_t>(index - history)];
}

bool Screen::isWrappedContinuation(int index) const
{
    if (m_inAlt || index <= 0 || index >= totalLines()) {
        return false;
    }
    const int scrollbackLines = static_cast<int>(m_scrollback.size());
    if (index < scrollbackLines) {
        return m_scrollback.wrapped(static_cast<std::size_t>(index));
    }
    return m_mainWrapped[index - scrollbackLines];
}

// Re-wrap state. Logical lines are assembled from the old physical lines
// one at a time and re-wrapped at the new width straight into the compacted
// `out`, so the history is never held uncompressed (a resize used to copy it
// three times at full width).
struct Screen::ReflowStream {
    static constexpr std::size_t Open = 1u;          // a logical line is being assembled
    static constexpr std::size_t CursorPending = 2u; // the cursor is on it

    Scrollback out;
    Line logical;
    Line chunk;
    // Output line (relative to the map base) where each input line starts.
    std::vector<long long> anchors;
    std::vector<std::size_t> chunkStarts;
    std::size_t logicalStart = 0; // output line of the logical line's first row
    std::size_t offset = 0;       // column of the current input line in it
    std::size_t previousSize = 0; // width of the previous input line
    std::size_t input = 0;        // input lines fed so far
    std::size_t cursorInput = 0;  // input line holding the cursor
    std::size_t cursorCol = 0;    // cursor column in the logical line
    std::size_t cursorNewPhys = 0;
    std::size_t cursorNewCol = 0;
    std::size_t state = 0;
};

void Screen::reflow()
{
    if (m_inAlt) {
        return;
    }
    const std::size_t screenRows = reflowScreenRows();
    const std::size_t oldHistory = m_scrollback.size();
    const std::size_t keep = reflowStart();
    const long long base = m_droppedLines;
    const std::vector<unsigned char> oldSizes = m_lineSizes;

    ReflowStream stream;
    Scrollback input = takeReflowInput(keep, screenRows);
    stream.out = std::move(m_scrollback); // the history lines kept as they are
    stream.cursorInput = oldHistory - keep + static_cast<std::size_t>(m_cursorRow);
    stream.anchors.reserve(input.size());
    Line phys;
    while (!input.empty()) {
        const bool wrapped = input.takeFront(phys);
        feedReflow(stream, phys, wrapped);
    }
    flushReflow(stream);

    std::size_t keepFrom = 0;
    redistribute(stream, keepFrom);
    m_droppedLines = base + static_cast<long long>(keepFrom);
    // Lines before `keep` did not move: the map starts after them.
    std::ranges::transform(stream.anchors, stream.anchors.begin(), [base](long long line) { return base + line; });
    m_reflowMap = ReflowMap{.base = base + static_cast<long long>(keep), .anchors = std::move(stream.anchors)};

    // Line sizes follow their rows: a double-size line keeps its size when
    // it is still on screen.
    m_lineSizes.assign(static_cast<std::size_t>(m_rows), SingleLine);
    std::ranges::for_each(std::views::iota(std::size_t{0}, oldSizes.size()), [&](std::size_t row) {
        const std::size_t index = oldHistory + row - keep;
        if (oldSizes[row] == SingleLine || index >= m_reflowMap.anchors.size()) {
            return;
        }
        const long long newRow = m_reflowMap.anchors[index] - lineAnchor(static_cast<int>(m_scrollback.size()));
        if (newRow >= 0 && newRow < m_rows) {
            m_lineSizes[static_cast<std::size_t>(newRow)] = oldSizes[row];
        }
    });
}

// Screen rows taking part in the reflow: down to the cursor or the last
// non-blank row, whichever is lower.
std::size_t Screen::reflowScreenRows() const
{
    std::size_t screenRows = std::min(m_main.size(), static_cast<std::size_t>(m_cursorRow + 1));
    for (std::size_t row = screenRows; row < m_main.size(); ++row) {
        if (!isBlankLine(m_main[row])) {
            screenRows = row + 1;
        }
    }
    return screenRows;
}

// First physical line that needs re-wrapping. History laid out at the
// current width re-wraps to itself, so when only the height changed (or the
// alternate screen closed) only the logical line reaching into the screen
// and the screen itself are re-wrapped; leaving vim no longer walks the
// whole history.
std::size_t Screen::reflowStart() const
{
    std::size_t start = m_scrollback.size();
    if (m_scrollback.uniformWidth() != m_cols) {
        return 0;
    }
    bool continues = !m_mainWrapped.empty() && m_mainWrapped.front();
    while (continues && start > 0) {
        --start;
        continues = m_scrollback.wrapped(start);
    }
    return start;
}

// The physical lines to re-wrap, oldest first: history from `keep` on, then
// the first `screenRows` screen rows. The whole history is moved, not copied.
Scrollback Screen::takeReflowInput(std::size_t keep, std::size_t screenRows)
{
    Scrollback input;
    if (keep == 0) {
        input = std::move(m_scrollback);
        m_scrollback = Scrollback{};
    } else {
        std::vector<std::pair<Line, bool>> tail(m_scrollback.size() - keep);
        std::ranges::for_each(std::views::reverse(tail), [this](std::pair<Line, bool> &line) {
            line.second = m_scrollback.takeBack(line.first);
        });
        std::ranges::for_each(tail, [&input](const std::pair<Line, bool> &line) {
            input.pushBack(line.first, line.second);
        });
    }
    std::ranges::for_each(std::views::iota(std::size_t{0}, screenRows), [&](std::size_t row) {
        input.pushBack(m_main[row], m_mainWrapped[row]);
    });
    return input;
}

// Add the next physical line: it either continues the logical line being
// assembled (soft wrap) or ends it and starts the next one.
void Screen::feedReflow(ReflowStream &stream, Line &phys, bool wrapped) const
{
    const std::size_t physSize = phys.size();
    if ((stream.state & ReflowStream::Open) == 0u) {
        stream.logical.swap(phys);
        stream.logicalStart = stream.out.size();
        stream.offset = 0;
        stream.state |= ReflowStream::Open;
    } else if (wrapped) {
        if (!stream.logical.empty() && isWrapPadding(stream.logical, stream.logical.size() - 1)) {
            stream.logical.pop_back();
        }
        stream.offset += stream.previousSize;
        stream.logical.insert(stream.logical.end(), phys.begin(), phys.end());
    } else {
        flushReflow(stream);
        stream.logical.swap(phys);
        stream.logicalStart = stream.out.size();
        stream.offset = 0;
        stream.state |= ReflowStream::Open;
    }
    stream.previousSize = physSize;
    stream.anchors.push_back(
        static_cast<long long>(stream.logicalStart + (stream.offset / static_cast<std::size_t>(m_cols)))
    );
    if (stream.input == stream.cursorInput) {
        stream.cursorCol = (wrapped ? stream.logical.size() - physSize : 0) + static_cast<std::size_t>(m_cursorCol);
        stream.state |= ReflowStream::CursorPending;
    }
    ++stream.input;
}

// End the logical line: trim its trailing padding (spaces at a wrap point
// are content, so only the end of the whole logical line is padding) and
// re-wrap it, keeping the cursor on its column.
void Screen::flushReflow(ReflowStream &stream) const
{
    if ((stream.state & ReflowStream::Open) == 0u) {
        return;
    }
    Line &line = stream.logical;
    // A wide character's spacer is part of it, not trailing padding.
    while (!line.empty() && line.back().ch == ' ' &&
           (!hasFlag(line.back(), CellFlag::WideSpacer) || isWrapPadding(line, line.size() - 1))) {
        line.pop_back();
    }
    stream.chunkStarts.clear();
    wrapLogicalLine(line, stream);
    if ((stream.state & ReflowStream::CursorPending) != 0u) {
        // Last chunk starting at or before the cursor column; columns past
        // it (cursor beyond the trimmed text) spill onto following rows.
        const auto cols = static_cast<std::size_t>(m_cols);
        const auto chunk = std::ranges::upper_bound(stream.chunkStarts, stream.cursorCol) - 1;
        const std::size_t offset = stream.cursorCol - *chunk;
        stream.cursorNewPhys = stream.logicalStart +
                               static_cast<std::size_t>(std::distance(stream.chunkStarts.begin(), chunk)) +
                               (offset / cols);
        stream.cursorNewCol = offset % cols;
    }
    stream.state = 0;
}

// Re-wrap one logical line at the new width. A wide character never
// straddles two rows: when its lead would land in the last column it moves
// to the next row and the gap becomes wrap padding.
void Screen::wrapLogicalLine(const Line &log, ReflowStream &stream) const
{
    const auto cols = static_cast<std::size_t>(m_cols);
    std::size_t start = 0;
    while (true) {
        stream.chunkStarts.push_back(start);
        std::size_t take = std::min(cols, log.size() - start);
        const bool more = start + take < log.size();
        if (more && take > 1 && hasFlag(log[start + take - 1], CellFlag::Wide)) {
            --take;
        }
        Line &line = stream.chunk;
        line.assign(
            log.begin() + static_cast<std::ptrdiff_t>(start),
            log.begin() + static_cast<std::ptrdiff_t>(start + take)
        );
        if (more && line.size() < cols) {
            line.push_back(Cell{.fg = 0, .bg = 0, .ch = ' ', .flags = CellFlag::WideSpacer});
        }
        line.resize(cols, blankCell());
        stream.out.pushBack(line, start != 0);
        start += take;
        if (!more) {
            break;
        }
    }
}

// The last `rows` re-wrapped lines become the screen, the rest the history
// (trimmed to the limit). The cursor stays attached to its logical line and
// column, which keeps it in place when the window grows instead of
// anchoring it to the bottom row of the newly larger screen.
void Screen::redistribute(ReflowStream &stream, std::size_t &keepFrom)
{
    Scrollback &out = stream.out;
    const std::size_t total = out.size();
    const auto rows = static_cast<std::size_t>(m_rows);
    // Cap only when a limit is set (0 = unlimited).
    const std::size_t maxTotal = m_maxScrollback > 0 ? rows + static_cast<std::size_t>(m_maxScrollback) : total;
    keepFrom = total > maxTotal ? total - maxTotal : 0;
    const std::size_t screenStart = total > rows ? total - rows : 0;
    m_main.assign(rows, Line(static_cast<std::size_t>(m_cols), blankCell()));
    m_mainWrapped.assign(rows, false);
    std::ranges::for_each(std::views::iota(screenStart, total) | std::views::reverse, [&](std::size_t index) {
        const std::size_t row = index - screenStart;
        m_mainWrapped[row] = out.takeBack(m_main[row]);
    });
    std::ranges::for_each(std::views::iota(std::size_t{0}, keepFrom), [&out](std::size_t) { out.popFront(); });
    m_scrollback = std::move(out);
    m_cursorRow = std::clamp(static_cast<int>(stream.cursorNewPhys) - static_cast<int>(screenStart), 0, m_rows - 1);
    m_cursorCol = std::min(static_cast<int>(stream.cursorNewCol), m_cols - 1);
    m_pendingWrap = false;
    m_scrollOffset = 0;
}

} // namespace konsoll
