#pragma once

#include "Cell.h"

#include <array>
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
    // Absolute screen position, ignoring origin mode (clamped to the grid).
    void moveCursor(int row, int col);
    // CUP/HVP/VPA position: relative to the scroll region in origin mode.
    void moveCursorAddressed(int row, int col);
    // CUU/CUD: stop at the scroll margin when the cursor is inside it.
    void cursorUp(int count);
    void cursorDown(int count);
    // DECSC/DECRC: position, pending wrap and origin mode, one slot per
    // screen (main / alternate), as in xterm.
    void saveCursor();
    void restoreCursor();
    void showCursor(bool visible);
    bool cursorVisible() const { return m_cursorVisible; }

    // Modes.
    bool originMode() const { return (m_modes & OriginMode) != 0; }
    void setOriginMode(bool on);
    bool autoWrap() const { return (m_modes & AutoWrapMode) != 0; }
    void setAutoWrap(bool on);
    bool insertMode() const { return (m_modes & InsertMode) != 0; }
    void setInsertMode(bool on);

    // Cell access for rendering.
    const Cell &cellAt(int row, int col) const;

    // Write a code point at the cursor (deferred wrap, advances cursor).
    // `pen` carries the SGR style (colors, flags, attrs, underline color,
    // hyperlink); its `ch` is ignored.
    void putChar(unsigned int ch, const Cell &pen);
    // Write a two-cell character (lead + spacer); wraps first when only the
    // last column is left, as xterm does.
    void putWideChar(unsigned int ch, const Cell &pen);
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
    void tab(int count = 1);
    void backTab(int count);
    void setTabStop();          // HTS
    void clearTabStop(int mode); // TBC: 0 at the cursor, 3 all
    void reverseIndex();

    // Erase.
    void eraseInDisplay(int mode); // 0,1,2,3
    void eraseInLine(int mode);    // 0,1,2
    void eraseChars(int count);    // ECH: blank `count` cells from the cursor

    // Background Color Erase: erased, inserted and scrolled-in cells take the
    // current SGR background (as in xterm/iTerm2), so full-screen apps can
    // paint their theme background with erase sequences.
    void setEraseBackground(unsigned int bg) { m_eraseBg = bg; }

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
    // `restoreCursor` (mode 1049) restores the main screen's DECSC cursor
    // before the main screen is re-wrapped, so the reflow keeps it in place.
    void leaveAltScreen(bool restoreCursor);
    bool inAltScreen() const { return m_inAlt; }

    // Full reset (ESC c): leaves the alternate screen and resets every mode.
    void fullReset();
    // Soft reset (DECSTR): modes, margins and the saved cursor; keeps content.
    void softReset();
    // DECALN: fill the screen with 'E' (screen alignment test).
    void alignmentTest();

private:
    static constexpr unsigned char OriginMode = 0x01;
    static constexpr unsigned char AutoWrapMode = 0x02;
    static constexpr unsigned char InsertMode = 0x04;
    static constexpr int SavedPendingWrap = 0x01;
    static constexpr int SavedOriginMode = 0x02;

    struct SavedCursor {
        int row{0};
        int col{0};
        int state{0}; // SavedPendingWrap | SavedOriginMode
    };

    struct ReflowedScreen {
        std::vector<Line> lines;
        std::vector<bool> wrapped;
    };

    void scrollUpOne(int top, int bottom);
    void scrollDownOne(int top, int bottom);
    void blankLine(int row);
    void resetTabStops(int fromCol);
    void applySavedCursor(const SavedCursor &saved);
    int previousCharCol() const;
    Cell eraseCell() const;
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
    std::vector<bool> m_tabStops; // per column: a tab stop is set
    int m_rows;
    int m_cols;
    unsigned int m_eraseBg = 0; // encoded like Cell::bg; 0 = default
    int m_cursorRow = 0;
    int m_cursorCol = 0;
    int m_scrollTop = 0;    // 0-based inclusive
    int m_scrollBottom = 0; // 0-based inclusive
    int m_maxScrollback = 0; // 0 = unlimited
    int m_scrollOffset = 0; // 0 = at the bottom
    std::array<SavedCursor, 2> m_savedCursors{}; // [0] main, [1] alternate
    bool m_cursorVisible = true;
    bool m_inAlt = false;
    bool m_pendingWrap = false;
    unsigned char m_modes = AutoWrapMode;
};

} // namespace konsoll
