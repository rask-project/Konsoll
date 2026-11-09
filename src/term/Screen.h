#pragma once

#include "Cell.h"

#include <deque>
#include <optional>
#include <vector>

namespace konsoll
{

using Line = std::vector<Cell>;

// The terminal screen model: a grid of cells plus cursor, scrollback and
// alternate-screen state. The ANSI parser (Terminal) drives it.
class Screen
{
public:
    explicit Screen(int rows, int cols);

    int rows() const { return m_rows; }
    int cols() const { return m_cols; }

    // Resize the grid, preserving the top-left content.
    void resize(int rows, int cols);

    // Cursor.
    int cursorRow() const { return m_cursorRow; }
    int cursorCol() const { return m_cursorCol; }
    void moveCursor(int row, int col);
    void saveCursor();
    void restoreCursor();
    void showCursor(bool visible);
    bool cursorVisible() const { return m_cursorVisible; }

    // Cell access for rendering.
    const Cell &cellAt(int row, int col) const;

    // Write a code point at the cursor (deferred wrap, advances cursor).
    void putChar(unsigned int ch, unsigned int fg, unsigned int bg, unsigned char flags);
    // Write a two-cell character (lead + spacer); wraps first when only the
    // last column is left, as xterm does.
    void putWideChar(unsigned int ch, unsigned int fg, unsigned int bg, unsigned char flags);
    // The code point last written on the cursor row (the cell left of the
    // cursor, or the cursor cell while a wrap is pending); nullopt at column 0.
    // Lets the parser fold a zero-width mark into the character it follows.
    std::optional<unsigned int> previousChar() const;
    void replacePreviousChar(unsigned int ch);
    // Turn the previously written narrow character into a wide one (emoji
    // presentation selector U+FE0F) when there is room on the row.
    void widenPreviousChar();

    // Line discipline.
    void lineFeed();
    void carriageReturn();
    void backspace();
    void tab();
    void reverseIndex();

    // Erase.
    void eraseInDisplay(int mode); // 0,1,2,3
    void eraseInLine(int mode);    // 0,1,2

    // Scrolling within the current region.
    void scrollUp(int count);
    void scrollDown(int count);
    void insertLines(int count);
    void deleteLines(int count);
    void setScrollRegion(int top, int bottom); // 1-based inclusive
    void resetScrollRegion();

    // Column operations at the cursor row.
    void deleteColumns(int count);
    void insertColumns(int count);

    // Scrollback (main screen only).
    int scrollbackSize() const { return static_cast<int>(m_scrollback.size()); }
    const Line &scrollbackLine(int index) const; // 0 = oldest
    void clearScrollback();

    // Scrollback limit: 0 = unlimited, > 0 = max lines.
    void setMaxScrollback(int max);
    int maxScrollback() const { return m_maxScrollback; }

    // Scroll view: `scrollOffset` is the number of lines the view is scrolled
    // up from the bottom (0 = at the bottom). `lineAt` addresses the whole
    // history (scrollback + screen) by absolute index, 0 = oldest.
    int totalLines() const;
    int maxScrollOffset() const;
    void scrollBy(int delta);
    void scrollTo(int offset);
    void scrollToBottom();
    int scrollOffset() const { return m_scrollOffset; }
    bool atBottom() const { return m_scrollOffset == 0; }
    const Line &lineAt(int index) const;
    // True when this physical row continues the logical line above it.
    bool isWrappedContinuation(int index) const;

    // Alternate screen.
    void enterAltScreen();
    void leaveAltScreen();
    bool inAltScreen() const { return m_inAlt; }

    // Full reset (ESC c).
    void fullReset();

private:
    struct ReflowedScreen {
        std::vector<Line> lines;
        std::vector<bool> wrapped;
    };

    void scrollUpOne(int top, int bottom);
    void scrollDownOne(int top, int bottom);
    void blankLine(int row);
    int previousCharCol() const;
    // Blank both halves of the wide pair cell `col` of the cursor row belongs
    // to, so a partial overwrite never leaves half a wide character behind.
    void breakWideAt(int col);
    void pushScrollback(const Line &line, bool wrapped);
    void lineFeedImpl(bool soft);
    void reflow();
    void collectPhysicalLines(std::vector<Line> &phys, std::vector<bool> &wrapped) const;
    std::vector<std::vector<Cell>> buildLogicalLines(
        const std::vector<Line> &phys, const std::vector<bool> &wrapped, size_t cursorPhys,
        size_t &cursorLogical, size_t &cursorLogicalCol) const;
    ReflowedScreen rewrapLogicalLines(
        const std::vector<std::vector<Cell>> &logical, size_t cursorLogical, size_t cursorLogicalCol,
        size_t &cursorNewPhys, size_t &cursorNewCol) const;
    void wrapLogicalLine(const std::vector<Cell> &log, ReflowedScreen &out, std::vector<size_t> &chunkStarts) const;
    void redistribute(const ReflowedScreen &rewrapped, size_t cursorNewPhys, size_t cursorNewCol);
    std::vector<Line> &grid() { return m_inAlt ? m_alt : m_main; }
    const std::vector<Line> &grid() const { return m_inAlt ? m_alt : m_main; }

    // Members ordered heaviest-first (grids, then 4-byte ints, then packed
    // 1-byte flags) so the layout stays padding-free under -Wpadded.
    std::vector<Line> m_main;
    std::vector<Line> m_alt;
    std::vector<Line> m_mainSaved;
    std::deque<Line> m_scrollback;
    // Reflow bookkeeping: `wrapped[i]` marks a physical line that is a soft
    // continuation of the line above it (right-margin wrap, not an explicit
    // CR/LF). Kept parallel to the grid rows and the scrollback so a resize
    // can re-wrap logical lines to the new width.
    std::vector<bool> m_mainWrapped;
    std::deque<bool> m_scrollbackWrapped;
    std::vector<bool> m_mainSavedWrapped;
    int m_rows;
    int m_cols;
    int m_cursorRow = 0;
    int m_cursorCol = 0;
    int m_savedRow = 0;
    int m_savedCol = 0;
    int m_scrollTop = 0;    // 0-based inclusive
    int m_scrollBottom = 0; // 0-based inclusive
    int m_maxScrollback = 0; // 0 = unlimited
    int m_scrollOffset = 0; // 0 = at the bottom
    bool m_cursorVisible = true;
    bool m_inAlt = false;
    bool m_pendingWrap = false;
};

} // namespace konsoll
