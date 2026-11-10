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

// Kitty's row / column diacritics (rowcolumn-diacritics.txt), sorted: the
// index of a diacritic is the row or column it encodes.
constexpr std::array<char32_t, 297> kPlaceholderDiacritics{
    0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346, 0x034A, 0x034B, 0x034C, 0x0350,
    0x0351, 0x0352, 0x0357, 0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369, 0x036A, 0x036B,
    0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595,
    0x0597, 0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1, 0x05A8, 0x05A9, 0x05AB, 0x05AC,
    0x05AF, 0x05C4, 0x0610, 0x0611, 0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658, 0x0659,
    0x065A, 0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8, 0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0,
    0x06E1, 0x06E2, 0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733, 0x0735, 0x0736, 0x073A,
    0x073D, 0x073F, 0x0740, 0x0741, 0x0743, 0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE,
    0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817, 0x0818, 0x0819, 0x081B, 0x081C, 0x081D, 0x081E, 0x081F,
    0x0820, 0x0821, 0x0822, 0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C, 0x082D, 0x0951,
    0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87, 0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17, 0x1A75,
    0x1A76, 0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D, 0x1B6E, 0x1B6F, 0x1B70, 0x1B71,
    0x1B72, 0x1B73, 0x1CD0, 0x1CD1, 0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4, 0x1DC5,
    0x1DC6, 0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1, 0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5, 0x1DD6, 0x1DD7,
    0x1DD8, 0x1DD9, 0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF, 0x1DE0, 0x1DE1, 0x1DE2, 0x1DE3, 0x1DE4,
    0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1, 0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7,
    0x20E9, 0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2, 0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7,
    0x2DE8, 0x2DE9, 0x2DEA, 0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2, 0x2DF3, 0x2DF4,
    0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA, 0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C,
    0xA67D, 0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5, 0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9,
    0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED, 0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7, 0xAAB8,
    0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23, 0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185,
    0x1D186, 0x1D187, 0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243, 0x1D244,
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

int placeholderIndex(char32_t diacritic)
{
    const auto *const found = std::ranges::lower_bound(kPlaceholderDiacritics, diacritic);
    return found != kPlaceholderDiacritics.end() && *found == diacritic
               ? static_cast<int>(found - kPlaceholderDiacritics.begin())
               : -1;
}

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
