#pragma once

#include "Cell.h"

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <ranges>
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
    // CUF/CUB: stop at the left/right margin when the cursor is inside it.
    void cursorForward(int count);
    void cursorBack(int count);
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
    // DECLRMM (?69) and DECSLRM: left/right margins (0-based inclusive).
    bool leftRightMarginMode() const { return (m_modes & LeftRightMarginMode) != 0; }
    void setLeftRightMarginMode(bool on);
    void setLeftRightMargins(int left, int right); // 1-based inclusive
    int leftMargin() const { return leftRightMarginMode() ? m_marginLeft : 0; }
    int rightMargin() const { return leftRightMarginMode() ? m_marginRight : m_cols - 1; }

    // DECBI / DECFI: move left / right, scrolling the margins' columns when
    // the cursor is at the left / right margin.
    void backIndex();
    void forwardIndex();
    // DECIC / DECDC: insert / delete columns at the cursor in the scroll
    // region, inside the margins.
    void insertColumnsInRegion(int count);
    void deleteColumnsInRegion(int count);

    // Rectangular operations (VT420), 0-based inclusive and clamped.
    struct Rect {
        int top{0};
        int left{0};
        int bottom{0};
        int right{0};
    };
    void fillRect(const Rect &rect, const Cell &cell);                // DECFRA
    void eraseRect(const Rect &rect, unsigned int protectedMask);     // DECERA / DECSERA
    void copyRect(const Rect &source, int top, int left);             // DECCRA
    // DECCARA / DECRARA: `change` applies to every cell of the rectangle,
    // or of the character stream from its top-left to its bottom-right.
    template <typename Change> void changeRect(const Rect &rect, bool stream, Change change);
    int checksumRect(const Rect &rect) const;                         // DECRQCRA

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
    // Erases skip cells whose attrs carry any of `protectedMask` (DECSCA /
    // SPA protection; 0 erases everything).
    void eraseInDisplay(int mode, unsigned int protectedMask = 0); // 0,1,2,3
    void eraseInLine(int mode, unsigned int protectedMask = 0);    // 0,1,2
    void eraseChars(int count, unsigned int protectedMask = 0);    // ECH: blank `count` cells from the cursor
    // SL (count > 0) / SR (count < 0): shift the scroll region sideways.
    void scrollHorizontally(int count);

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
    int scrollTop() const { return m_scrollTop; }       // 0-based inclusive
    int scrollBottom() const { return m_scrollBottom; } // 0-based inclusive
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

    // Absolute line numbers that survive scrolling and history trimming, for
    // anchoring images: lineAnchor(i) is the line lineAt(i) shows,
    // rowAnchor(r) screen row r. The alternate screen counts its own
    // scrolls.
    long long lineAnchor(int index) const { return (m_inAlt ? m_altScrolled : m_droppedLines) + index; }
    long long rowAnchor(int row) const
    {
        return lineAnchor((m_inAlt ? 0 : static_cast<int>(m_scrollback.size())) + row);
    }

    // Alternate screen.
    void enterAltScreen();
    // `restoreCursor` (mode 1049) restores the main screen's DECSC cursor
    // before the main screen is re-wrapped, so the reflow keeps it in place.
    void leaveAltScreen(bool restoreCursor);
    bool inAltScreen() const { return m_inAlt; }

    // DEC line size of a screen row: 0 single, 1 double width (DECDWL),
    // 2 / 3 top / bottom half of double height (DECDHL). lineSize() takes a
    // lineAt() index; history lines are single.
    static constexpr int SingleLine = 0;
    static constexpr int DoubleWidthLine = 1;
    static constexpr int DoubleHeightTop = 2;
    static constexpr int DoubleHeightBottom = 3;
    int lineSize(int index) const;
    void setLineSize(int size); // the cursor row

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
    static constexpr unsigned char LeftRightMarginMode = 0x08;
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
    int wrapColumn() const;
    // Columns usable on the cursor row: half of them on a double-size line.
    int rowColumns(int row) const;
    void rotateLineSizes(int top, int bottom, int count);
    void resetLineSizes();
    void wrapToNextLine();
    bool insideMargins() const { return m_cursorCol >= leftMargin() && m_cursorCol <= rightMargin(); }
    bool hasSideMargins() const { return leftMargin() > 0 || rightMargin() < m_cols - 1; }
    // Move rows [top, bottom] up (count > 0) / down within columns
    // [left, right], blanking what is vacated.
    void shiftRows(int top, int bottom, int left, int right, int count);
    // Move columns [left, right] of rows [top, bottom] left (count > 0) /
    // right, blanking what is vacated.
    void shiftColumns(int top, int bottom, int left, int right, int count);
    Rect clampRect(const Rect &rect) const;
    void eraseCells(int row, int first, int last, unsigned int protectedMask);
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
    // Line sizes of the current grid's rows, and the main grid's while the
    // alternate screen is shown.
    std::vector<unsigned char> m_lineSizes;
    std::vector<unsigned char> m_mainSavedLineSizes;
    long long m_droppedLines = 0; // history lines removed from the top so far
    long long m_altScrolled = 0;  // alternate-screen scroll-ups so far
    int m_rows;
    int m_cols;
    unsigned int m_eraseBg = 0; // encoded like Cell::bg; 0 = default
    int m_cursorRow = 0;
    int m_cursorCol = 0;
    int m_scrollTop = 0;    // 0-based inclusive
    int m_scrollBottom = 0; // 0-based inclusive
    int m_maxScrollback = 0; // 0 = unlimited
    int m_scrollOffset = 0; // 0 = at the bottom
    int m_marginLeft = 0;   // DECSLRM, 0-based inclusive
    int m_marginRight = 0;
    std::array<SavedCursor, 2> m_savedCursors{}; // [0] main, [1] alternate
    bool m_cursorVisible = true;
    bool m_inAlt = false;
    bool m_pendingWrap = false;
    unsigned char m_modes = AutoWrapMode;
};

template <typename Change> void Screen::changeRect(const Rect &rect, bool stream, Change change)
{
    const Rect r = clampRect(rect);
    std::ranges::for_each(std::views::iota(r.top, r.bottom + 1), [&](int row) {
        // In stream mode (DECSACE 0/1) the first and last rows run from / to
        // the line edges, like a text selection.
        const int first = stream && row != r.top ? 0 : r.left;
        const int last = stream && row != r.bottom ? m_cols - 1 : r.right;
        Line &line = grid()[row];
        std::for_each(line.begin() + first, line.begin() + last + 1, change);
    });
}

} // namespace konsoll
