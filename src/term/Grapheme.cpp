#include "Grapheme.h"

#include "CharWidth.h"

#include <QChar>

#include <algorithm>
#include <cstdint>
#include <array>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace konsoll
{

namespace
{

struct ClusterEntry {
    std::u32string codePoints;
    std::int64_t width; // 1 or 2; word-sized so the entry has no padding
};

// Cells hold ids into this table from any terminal; the renderer reads it on
// the render thread, so it is guarded.
class ClusterTable
{
public:
    unsigned int intern(const std::u32string &cluster, int width)
    {
        {
            const std::shared_lock lock(m_mutex);
            const auto found = m_ids.find(cluster);
            if (found != m_ids.end()) {
                return found->second;
            }
        }
        const std::unique_lock lock(m_mutex);
        const auto [entry, inserted] = m_ids.try_emplace(cluster, kClusterBase + static_cast<unsigned int>(m_entries.size()));
        if (inserted) {
            m_entries.push_back(ClusterEntry{.codePoints = cluster, .width = width});
        }
        return entry->second;
    }

    ClusterEntry entry(unsigned int ch) const
    {
        const std::shared_lock lock(m_mutex);
        const std::size_t index = ch - kClusterBase;
        return index < m_entries.size() ? m_entries[index] : ClusterEntry{.codePoints = U"�", .width = 1};
    }

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::u32string, unsigned int> m_ids;
    std::deque<ClusterEntry> m_entries;
};

ClusterTable &table()
{
    static ClusterTable instance;
    return instance;
}

bool inRange(char32_t c, char32_t first, char32_t last)
{
    return c >= first && c <= last;
}

bool isRegionalIndicator(char32_t c)
{
    return inRange(c, 0x1F1E6, 0x1F1FF);
}

bool isEmojiModifier(char32_t c)
{
    return inRange(c, 0x1F3FB, 0x1F3FF);
}

// Extended_Pictographic, approximated by the emoji blocks and the symbols
// with emoji forms.
bool isPictographic(char32_t c)
{
    struct Range {
        char32_t first;
        char32_t last;
    };
    static constexpr std::array kRanges{
        Range{.first = 0x00A9, .last = 0x00A9},   Range{.first = 0x00AE, .last = 0x00AE},
        Range{.first = 0x203C, .last = 0x203C},   Range{.first = 0x2049, .last = 0x2049},
        Range{.first = 0x2122, .last = 0x2122},   Range{.first = 0x2139, .last = 0x2139},
        Range{.first = 0x2194, .last = 0x21AA},   Range{.first = 0x231A, .last = 0x23FF},
        Range{.first = 0x24C2, .last = 0x24C2},   Range{.first = 0x25AA, .last = 0x25FE},
        Range{.first = 0x2600, .last = 0x27BF},   Range{.first = 0x2934, .last = 0x2935},
        Range{.first = 0x2B05, .last = 0x2B55},   Range{.first = 0x3030, .last = 0x3030},
        Range{.first = 0x303D, .last = 0x303D},   Range{.first = 0x3297, .last = 0x3299},
        Range{.first = 0x1F000, .last = 0x1FAFF},
    };
    return std::ranges::any_of(kRanges, [c](const Range &r) { return inRange(c, r.first, r.last); });
}

// Grapheme_Cluster_Break=Extend / SpacingMark / ZWJ.
bool isExtend(char32_t c)
{
    if (c == 0x200D || inRange(c, 0xFE00, 0xFE0F) || inRange(c, 0xE0100, 0xE01EF) || isEmojiModifier(c) ||
        inRange(c, 0xE0020, 0xE007F)) {
        return true;
    }
    const QChar::Category category = QChar::category(c);
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing ||
           category == QChar::Mark_SpacingCombining;
}

// Hangul syllable types: L, V, T leading / vowel / trailing jamo, LV / LVT
// precomposed syllables.
enum class Hangul : std::uint8_t {
    None,
    L,
    V,
    T,
    LV,
    LVT
};

Hangul hangulType(char32_t c)
{
    if (inRange(c, 0x1100, 0x115F) || inRange(c, 0xA960, 0xA97C)) {
        return Hangul::L;
    }
    if (inRange(c, 0x1160, 0x11A7) || inRange(c, 0xD7B0, 0xD7C6)) {
        return Hangul::V;
    }
    if (inRange(c, 0x11A8, 0x11FF) || inRange(c, 0xD7CB, 0xD7FB)) {
        return Hangul::T;
    }
    if (inRange(c, 0xAC00, 0xD7A3)) {
        return (c - 0xAC00) % 28 == 0 ? Hangul::LV : Hangul::LVT;
    }
    return Hangul::None;
}

bool joinsHangul(char32_t last, char32_t next)
{
    const Hangul a = hangulType(last);
    const Hangul b = hangulType(next);
    switch (a) {
    case Hangul::L:
        return b == Hangul::L || b == Hangul::V || b == Hangul::LV || b == Hangul::LVT;
    case Hangul::LV:
    case Hangul::V:
        return b == Hangul::V || b == Hangul::T;
    case Hangul::LVT:
    case Hangul::T:
        return b == Hangul::T;
    default:
        return false;
    }
}

} // namespace

unsigned int internCluster(const std::u32string &cluster, int width)
{
    return table().intern(cluster, width);
}

std::u32string cellCodePoints(unsigned int ch)
{
    if (ch < kClusterBase) {
        return std::u32string(1, static_cast<char32_t>(ch));
    }
    return table().entry(ch).codePoints;
}

QString cellText(unsigned int ch)
{
    const std::u32string codePoints = cellCodePoints(ch);
    return QString::fromUcs4(codePoints.data(), static_cast<qsizetype>(codePoints.size()));
}

int clusterWidth(unsigned int ch)
{
    return ch < kClusterBase ? 0 : static_cast<int>(table().entry(ch).width);
}

bool extendsCluster(const std::u32string &previous, char32_t next)
{
    if (previous.empty()) {
        return false;
    }
    if (isExtend(next)) {
        return true;
    }
    const char32_t last = previous.back();
    if (last == 0x200D && isPictographic(next)) {
        return true; // emoji ZWJ sequence
    }
    if (isRegionalIndicator(next)) {
        // Flags pair regional indicators: join an odd run.
        const auto run = std::ranges::count_if(previous, isRegionalIndicator);
        return run % 2 == 1 && isRegionalIndicator(last);
    }
    return joinsHangul(last, next);
}

int graphemeWidth(const std::u32string &cluster)
{
    if (cluster.empty()) {
        return 1;
    }
    const bool emoji = std::ranges::any_of(cluster, [](char32_t c) {
        return c == 0xFE0F || isEmojiModifier(c) || c == 0x200D;
    });
    const bool flag = cluster.size() >= 2 && isRegionalIndicator(cluster[0]) && isRegionalIndicator(cluster[1]);
    return emoji || flag || isWideChar(cluster.front()) ? 2 : 1;
}

} // namespace konsoll
