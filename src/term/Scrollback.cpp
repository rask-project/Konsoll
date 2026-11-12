#include "Scrollback.h"

#include <algorithm>
#include <ranges>
#include <utility>

#include <sys/mman.h>

#include <cstring>

namespace konsoll
{

namespace
{
// Style fields present in a run (the ones that are not zero).
namespace StyleField
{
inline constexpr unsigned int Fg = 1u << 0;
inline constexpr unsigned int Bg = 1u << 1;
inline constexpr unsigned int Ul = 1u << 2;
inline constexpr unsigned int Link = 1u << 3;
inline constexpr unsigned int Flags = 1u << 4;
inline constexpr unsigned int Attrs = 1u << 5;
} // namespace StyleField

// A block's mapping; only the pages written to take memory. A longer line
// gets a block of its own, rounded up to whole pages.
constexpr std::size_t kBlockBytes = std::size_t{256} << 10U;
constexpr std::size_t kPageBytes = std::size_t{16} << 10U;
constexpr std::size_t kOffsetBytes = sizeof(std::uint32_t);

void putVarint(std::vector<unsigned char> &out, std::uint32_t value)
{
    while (value >= 0x80u) {
        out.push_back(static_cast<unsigned char>((value & 0x7Fu) | 0x80u));
        value >>= 7u;
    }
    out.push_back(static_cast<unsigned char>(value));
}

// Colors: a truecolor value (0x80000000 | rgb) rotated left by one bit takes
// four varint bytes instead of five; palette indexes stay small.
std::uint32_t packColor(std::uint32_t color)
{
    return (color << 1u) | (color >> 31u);
}

std::uint32_t unpackColor(std::uint32_t packed)
{
    return (packed >> 1u) | (packed << 31u);
}

class Reader
{
public:
    explicit Reader(std::span<const unsigned char> bytes) : m_bytes(bytes) {}

    std::uint32_t varint()
    {
        std::uint32_t value = 0;
        unsigned int shift = 0;
        while (m_pos < m_bytes.size() && shift < 32u) {
            const unsigned int byte = m_bytes[m_pos++];
            value |= (byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0u) {
                break;
            }
            shift += 7u;
        }
        return value;
    }

    unsigned char byte() { return m_pos < m_bytes.size() ? m_bytes[m_pos++] : 0; }

private:
    std::span<const unsigned char> m_bytes;
    std::size_t m_pos = 0;
};

bool sameStyle(const Cell &a, const Cell &b)
{
    return a.fg == b.fg && a.bg == b.bg && a.ul == b.ul && a.link == b.link && a.flags == b.flags && a.attrs == b.attrs;
}

void putStyle(std::vector<unsigned char> &out, const Cell &style)
{
    const unsigned int mask = (style.fg != 0u ? StyleField::Fg : 0u) | (style.bg != 0u ? StyleField::Bg : 0u) |
                              (style.ul != 0u ? StyleField::Ul : 0u) | (style.link != 0u ? StyleField::Link : 0u) |
                              (style.flags != 0u ? StyleField::Flags : 0u) |
                              (style.attrs != 0u ? StyleField::Attrs : 0u);
    out.push_back(static_cast<unsigned char>(mask));
    if ((mask & StyleField::Fg) != 0u) {
        putVarint(out, packColor(style.fg));
    }
    if ((mask & StyleField::Bg) != 0u) {
        putVarint(out, packColor(style.bg));
    }
    if ((mask & StyleField::Ul) != 0u) {
        putVarint(out, packColor(style.ul));
    }
    if ((mask & StyleField::Link) != 0u) {
        putVarint(out, style.link);
    }
    if ((mask & StyleField::Flags) != 0u) {
        out.push_back(style.flags);
    }
    if ((mask & StyleField::Attrs) != 0u) {
        out.push_back(style.attrs);
    }
}

Cell readStyle(Reader &in)
{
    const unsigned int mask = in.byte();
    Cell style;
    style.fg = (mask & StyleField::Fg) != 0u ? unpackColor(in.varint()) : 0u;
    style.bg = (mask & StyleField::Bg) != 0u ? unpackColor(in.varint()) : 0u;
    style.ul = (mask & StyleField::Ul) != 0u ? unpackColor(in.varint()) : 0u;
    style.link = (mask & StyleField::Link) != 0u ? static_cast<unsigned short>(in.varint()) : 0;
    style.flags = (mask & StyleField::Flags) != 0u ? in.byte() : 0;
    style.attrs = (mask & StyleField::Attrs) != 0u ? in.byte() : 0;
    return style;
}

// Line layout: varint (content << 1 | wrapped), varint trailing blanks, then
// the runs. A blank line's first byte is 0 or 1, so the wrap flag and
// whether a line has content are read from its first byte alone.
// Returns whether the line has content.
bool encodeLine(const Line &line, bool wrapped, std::vector<unsigned char> &out)
{
    const auto tail = std::ranges::find_last_if(line, [](const Cell &cell) { return cell != Cell{}; });
    const auto content = tail.empty() ? std::size_t{0} : static_cast<std::size_t>(tail.begin() - line.begin()) + 1;
    putVarint(out, static_cast<std::uint32_t>((content << 1u) | (wrapped ? 1u : 0u)));
    putVarint(out, static_cast<std::uint32_t>(line.size() - content));
    const auto end = line.begin() + static_cast<std::ptrdiff_t>(content);
    auto run = line.begin();
    while (run != end) {
        const Cell &style = *run;
        const auto runEnd =
            std::find_if(std::next(run), end, [&style](const Cell &cell) { return !sameStyle(cell, style); });
        putVarint(out, static_cast<std::uint32_t>(runEnd - run));
        putStyle(out, style);
        std::for_each(run, runEnd, [&out](const Cell &cell) { putVarint(out, cell.ch); });
        run = runEnd;
    }
    return content > 0;
}

bool decodeLine(std::span<const unsigned char> bytes, Line &line)
{
    Reader in(bytes);
    const std::uint32_t head = in.varint();
    const std::size_t content = head >> 1u;
    const std::size_t blanks = in.varint();
    line.assign(content + blanks, Cell{});
    std::size_t col = 0;
    while (col < content) {
        const std::size_t run = std::min<std::size_t>(in.varint(), content - col);
        if (run == 0) {
            break; // malformed: never loop without progress
        }
        Cell cell = readStyle(in);
        std::ranges::generate_n(line.begin() + static_cast<std::ptrdiff_t>(col), static_cast<std::ptrdiff_t>(run), [&] {
            cell.ch = in.varint();
            return cell;
        });
        col += run;
    }
    return (head & 1u) != 0u;
}

bool hasContent(std::span<const unsigned char> bytes)
{
    return !bytes.empty() && bytes.front() > 1u;
}
} // namespace

void Scrollback::Unmap::operator()(unsigned char *data) const
{
    munmap(data, size);
}

std::uint32_t Scrollback::Block::offset(std::size_t line) const
{
    std::uint32_t value = 0;
    std::memcpy(&value, data.get() + capacity - ((line + 1) * kOffsetBytes), kOffsetBytes);
    return value;
}

std::span<const unsigned char> Scrollback::Block::line(std::size_t line) const
{
    const std::size_t begin = offset(line);
    const std::size_t end = line + 1 < lines ? offset(line + 1) : used;
    return {data.get() + begin, end - begin};
}

bool Scrollback::Block::fits(std::size_t bytes) const
{
    return used + bytes + ((lines + 1) * kOffsetBytes) <= capacity;
}

void Scrollback::Block::append(std::span<const unsigned char> bytes)
{
    const auto start = static_cast<std::uint32_t>(used);
    std::memcpy(data.get() + capacity - ((lines + 1) * kOffsetBytes), &start, kOffsetBytes);
    std::ranges::copy(bytes, data.get() + used);
    used += bytes.size();
    ++lines;
}

std::optional<Scrollback::Block> Scrollback::mapBlock(std::size_t minimum, std::size_t first)
{
    const std::size_t capacity = std::max(kBlockBytes, (minimum + kPageBytes - 1) / kPageBytes * kPageBytes);
    void *mapped = mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED) {
        return std::nullopt;
    }
    Block block;
    block.data = std::unique_ptr<unsigned char, Unmap>(static_cast<unsigned char *>(mapped), Unmap{.size = capacity});
    block.capacity = capacity;
    block.first = first;
    return block;
}

Scrollback::Scrollback(Scrollback &&other) noexcept
    : m_blocks(std::exchange(other.m_blocks, {}))
    , m_size(std::exchange(other.m_size, 0))
    , m_front(std::exchange(other.m_front, 0))
    , m_contentLines(std::exchange(other.m_contentLines, 0))
    , m_width(std::exchange(other.m_width, kNoWidth))
{}

Scrollback &Scrollback::operator=(Scrollback &&other) noexcept
{
    if (this != &other) {
        m_blocks = std::exchange(other.m_blocks, {});
        m_size = std::exchange(other.m_size, 0);
        m_front = std::exchange(other.m_front, 0);
        m_contentLines = std::exchange(other.m_contentLines, 0);
        m_width = std::exchange(other.m_width, kNoWidth);
    }
    return *this;
}

Scrollback::~Scrollback() = default;

void Scrollback::pushBack(const Line &line, bool wrapped)
{
    m_scratch.clear();
    const bool content = encodeLine(line, wrapped, m_scratch);
    if (m_blocks.empty() || !m_blocks.back().fits(m_scratch.size())) {
        const std::size_t first = m_blocks.empty() ? 0 : m_blocks.back().first + m_blocks.back().lines;
        std::optional<Block> block = mapBlock(m_scratch.size() + kOffsetBytes, first);
        if (!block) {
            return; // out of address space: the line is lost, not the session
        }
        m_blocks.push_back(std::move(*block));
    }
    m_blocks.back().append(m_scratch);
    if (content) {
        ++m_contentLines;
    }
    const auto width = static_cast<std::ptrdiff_t>(line.size());
    if (m_size == 0) {
        m_width = width;
    } else if (m_width != width) {
        m_width = kMixedWidth;
    }
    ++m_size;
}

void Scrollback::popFront()
{
    if (m_size == 0) {
        return;
    }
    if (hasContent(encoded(0))) {
        --m_contentLines;
    }
    ++m_front;
    --m_size;
    if (m_front == m_blocks.front().lines) {
        m_blocks.pop_front();
        m_front = 0;
    }
}

bool Scrollback::takeFront(Line &line)
{
    if (m_size == 0) {
        line.clear();
        return false;
    }
    const bool wrapped = decodeLine(encoded(0), line);
    popFront();
    return wrapped;
}

bool Scrollback::takeBack(Line &line)
{
    if (m_size == 0) {
        line.clear();
        return false;
    }
    Block &last = m_blocks.back();
    const std::span<const unsigned char> bytes = last.line(last.lines - 1);
    const bool wrapped = decodeLine(bytes, line);
    if (hasContent(bytes)) {
        --m_contentLines;
    }
    last.used = last.offset(last.lines - 1);
    --last.lines;
    --m_size;
    if (last.lines == (m_blocks.size() == 1 ? m_front : 0)) {
        m_blocks.pop_back();
        if (m_blocks.empty()) {
            m_front = 0;
        }
    }
    return wrapped;
}

void Scrollback::clear()
{
    reset();
}

void Scrollback::reset()
{
    // Swap rather than clear(): a cleared deque keeps its block map.
    std::deque<Block>().swap(m_blocks);
    std::vector<unsigned char>().swap(m_scratch);
    m_size = 0;
    m_front = 0;
    m_contentLines = 0;
    m_width = kNoWidth;
}

void Scrollback::line(std::size_t index, Line &line) const
{
    if (index >= m_size) {
        line.clear();
        return;
    }
    decodeLine(encoded(index), line);
}

bool Scrollback::wrapped(std::size_t index) const
{
    if (index >= m_size) {
        return false;
    }
    const std::span<const unsigned char> bytes = encoded(index);
    return !bytes.empty() && (bytes.front() & 1u) != 0u;
}

std::size_t Scrollback::memoryUsage() const
{
    // Pages of a block are only backed once written: count what is in use.
    return std::ranges::fold_left(m_blocks, std::size_t{0}, [](std::size_t total, const Block &block) {
        return total + block.used + (block.lines * kOffsetBytes);
    });
}

std::span<const unsigned char> Scrollback::encoded(std::size_t index) const
{
    const std::size_t absolute = m_blocks.front().first + m_front + index;
    // The last block starting at or before the line.
    const auto block =
        std::ranges::upper_bound(m_blocks, absolute, std::ranges::less{}, [](const Block &b) { return b.first; }) - 1;
    return block->line(absolute - block->first);
}

} // namespace konsoll
