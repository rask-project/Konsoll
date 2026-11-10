// Screen: left/right margins (DECSLRM) and the VT420 rectangular
// operations. Split from Screen.cpp.

#include "Screen.h"

#include <algorithm>
#include <ranges>

namespace konsoll
{

void Screen::setLeftRightMarginMode(bool on)
{
    m_modes = static_cast<unsigned char>(on ? (m_modes | LeftRightMarginMode) : (m_modes & ~LeftRightMarginMode));
    m_marginLeft = 0;
    m_marginRight = m_cols - 1;
}

void Screen::setLeftRightMargins(int left, int right) // NOLINT: left/right are inherently swappable
{
    if (!leftRightMarginMode()) {
        return;
    }
    const int l = std::clamp(left - 1, 0, m_cols - 1);
    const int r = std::clamp(right - 1, 0, m_cols - 1);
    if (l >= r) {
        return;
    }
    m_marginLeft = l;
    m_marginRight = r;
    moveCursorAddressed(0, 0); // DECSLRM homes the cursor
}

void Screen::cursorForward(int count)
{
    const int limit = m_cursorCol <= rightMargin() ? rightMargin() : m_cols - 1;
    moveCursor(m_cursorRow, std::min(m_cursorCol + std::max(count, 1), limit));
}

void Screen::cursorBack(int count)
{
    const int limit = m_cursorCol >= leftMargin() ? leftMargin() : 0;
    moveCursor(m_cursorRow, std::max(m_cursorCol - std::max(count, 1), limit));
}

Screen::Rect Screen::clampRect(const Rect &rect) const
{
    return Rect{
        .top = std::clamp(rect.top, 0, m_rows - 1),
        .left = std::clamp(rect.left, 0, m_cols - 1),
        .bottom = std::clamp(rect.bottom, 0, m_rows - 1),
        .right = std::clamp(rect.right, 0, m_cols - 1),
    };
}

void Screen::shiftRows(int top, int bottom, int left, int right, int count)
{
    const int height = bottom - top + 1;
    const int n = std::min(std::abs(count), height);
    if (n <= 0 || left > right) {
        return;
    }
    auto &g = grid();
    const Cell blank = eraseCell();
    const auto copyColumns = [&](int from, int to) {
        std::copy(g[from].begin() + left, g[from].begin() + right + 1, g[to].begin() + left);
    };
    const auto blankColumns = [&](int row) { std::fill(g[row].begin() + left, g[row].begin() + right + 1, blank); };
    if (count > 0) {
        std::ranges::for_each(std::views::iota(top, bottom - n + 1), [&](int row) { copyColumns(row + n, row); });
        std::ranges::for_each(std::views::iota(bottom - n + 1, bottom + 1), blankColumns);
    } else {
        std::ranges::for_each(std::views::iota(top + n, bottom + 1) | std::views::reverse,
                              [&](int row) { copyColumns(row - n, row); });
        std::ranges::for_each(std::views::iota(top, top + n), blankColumns);
    }
}

void Screen::shiftColumns(int top, int bottom, int left, int right, int count)
{
    const int n = std::min(std::abs(count), right - left + 1);
    if (n <= 0 || top > bottom) {
        return;
    }
    const Cell blank = eraseCell();
    std::ranges::for_each(std::views::iota(top, bottom + 1), [&](int row) {
        Line &line = grid()[row];
        const auto first = line.begin() + left;
        const auto last = line.begin() + right + 1;
        if (count > 0) {
            std::move(first + n, last, first);
            std::fill(last - n, last, blank);
        } else {
            std::move_backward(first, last - n, last);
            std::fill(first, first + n, blank);
        }
    });
}

void Screen::backIndex()
{
    if (m_cursorCol == leftMargin()) {
        shiftColumns(m_scrollTop, m_scrollBottom, leftMargin(), rightMargin(), -1);
    } else if (m_cursorCol > 0) {
        --m_cursorCol;
    }
    m_pendingWrap = false;
}

void Screen::forwardIndex()
{
    if (m_cursorCol == rightMargin()) {
        shiftColumns(m_scrollTop, m_scrollBottom, leftMargin(), rightMargin(), 1);
    } else if (m_cursorCol < m_cols - 1) {
        ++m_cursorCol;
    }
    m_pendingWrap = false;
}

void Screen::insertColumnsInRegion(int count)
{
    if (!insideMargins() || m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom) {
        return;
    }
    shiftColumns(m_scrollTop, m_scrollBottom, m_cursorCol, rightMargin(), -std::max(count, 1));
}

void Screen::deleteColumnsInRegion(int count)
{
    if (!insideMargins() || m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom) {
        return;
    }
    shiftColumns(m_scrollTop, m_scrollBottom, m_cursorCol, rightMargin(), std::max(count, 1));
}

void Screen::fillRect(const Rect &rect, const Cell &cell)
{
    changeRect(rect, false, [&cell](Cell &target) { target = cell; });
}

void Screen::eraseRect(const Rect &rect, unsigned int protectedMask)
{
    const Cell blank = eraseCell();
    changeRect(rect, false, [&blank, &protectedMask](Cell &target) {
        if ((target.attrs & protectedMask) == 0u) {
            target = blank;
        }
    });
}

void Screen::copyRect(const Rect &source, int top, int left)
{
    const Rect src = clampRect(source);
    if (src.top > src.bottom || src.left > src.right) {
        return;
    }
    // Copy through a buffer: the areas may overlap.
    std::vector<Line> buffer;
    std::ranges::for_each(std::views::iota(src.top, src.bottom + 1), [&](int row) {
        buffer.emplace_back(grid()[row].begin() + src.left, grid()[row].begin() + src.right + 1);
    });
    std::ranges::for_each(std::views::iota(0, static_cast<int>(buffer.size())), [&](int i) {
        const int row = top + i;
        if (row < 0 || row >= m_rows) {
            return;
        }
        const Line &cells = buffer[static_cast<std::size_t>(i)];
        const int width = std::min(static_cast<int>(cells.size()), m_cols - left);
        if (width > 0 && left >= 0) {
            std::copy_n(cells.begin(), width, grid()[row].begin() + left);
        }
    });
}

int Screen::checksumRect(const Rect &rect) const
{
    const Rect r = clampRect(rect);
    unsigned int sum = 0;
    std::ranges::for_each(std::views::iota(r.top, r.bottom + 1), [&](int row) {
        const Line &line = grid()[row];
        std::for_each(line.begin() + r.left, line.begin() + r.right + 1,
                      [&sum](const Cell &cell) { sum += cell.ch; });
    });
    return static_cast<int>((0u - sum) & 0xffffu); // xterm: the negated sum, 16 bits
}

} // namespace konsoll
