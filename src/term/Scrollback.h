#pragma once

#include "Cell.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace konsoll
{

using Line = std::vector<Cell>;

// The main screen's history, compacted so an unlimited scrollback stays
// affordable. A line is stored as its content up to the last non-blank cell,
// the count of blank cells after it (its width is kept exactly) and its
// soft-wrap flag. The content is a list of style runs (cells sharing every
// attribute but the code point): the run length, the style fields that are
// not zero, then the code points, all as variable-length integers. Plain
// text costs about one byte per character instead of sizeof(Cell) per
// column, and a blank line two bytes.
//
// Lines live in blocks of memory mapped straight from the system (256 KiB,
// text growing from the start and the line index from the end). Freed
// blocks are unmapped, so trimming, clearing and closing a session give the
// memory back at once: the system allocator keeps freed memory mapped, even
// in multi-megabyte blocks, so a malloc'd history never shrank.
class Scrollback
{
public:
    Scrollback() = default;
    Scrollback(const Scrollback &) = delete;
    Scrollback &operator=(const Scrollback &) = delete;
    Scrollback(Scrollback &&other) noexcept;
    Scrollback &operator=(Scrollback &&other) noexcept;
    ~Scrollback();

    std::size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    // Lines holding anything but blank cells.
    std::size_t contentLines() const { return m_contentLines; }
    // The width every stored line has, or -1 when they differ (or none is
    // stored).
    int uniformWidth() const { return m_width >= 0 ? static_cast<int>(m_width) : -1; }

    void pushBack(const Line &line, bool wrapped);
    void popFront();
    // Decode the oldest / newest line into `line`, remove it and return its
    // wrap flag.
    bool takeFront(Line &line);
    bool takeBack(Line &line);
    // Drop every line and give the memory back to the system.
    void clear();

    // Decode line `index` (0 = oldest) into `line`, at its stored width.
    void line(std::size_t index, Line &line) const;
    // True when line `index` continues the line before it (soft wrap).
    bool wrapped(std::size_t index) const;
    // Bytes held by the encoded lines and their index.
    std::size_t memoryUsage() const;

private:
    // No member initializer: clang would not see it as default-constructible
    // inside Scrollback, deleting Block's default constructor.
    struct Unmap {
        std::size_t size;
        void operator()(unsigned char *data) const;
    };
    // [line bytes ->        free        <- line offsets (uint32)]
    struct Block {
        std::unique_ptr<unsigned char, Unmap> data;
        std::size_t capacity = 0;
        std::size_t used = 0;  // bytes of line data
        std::size_t lines = 0; // lines stored (dropped ones included)
        std::size_t first = 0; // absolute number of the block's first line

        std::uint32_t offset(std::size_t line) const;
        std::span<const unsigned char> line(std::size_t line) const;
        bool fits(std::size_t bytes) const;
        void append(std::span<const unsigned char> bytes);
    };

    static constexpr std::ptrdiff_t kNoWidth = -1;
    static constexpr std::ptrdiff_t kMixedWidth = -2;

    static std::optional<Block> mapBlock(std::size_t minimum, std::size_t first);
    std::span<const unsigned char> encoded(std::size_t index) const;
    void reset();

    std::deque<Block> m_blocks;
    std::vector<unsigned char> m_scratch; // the line being encoded
    std::size_t m_size = 0;
    std::size_t m_front = 0; // lines already dropped from the first block
    std::size_t m_contentLines = 0;
    std::ptrdiff_t m_width = kNoWidth;
};

} // namespace konsoll
