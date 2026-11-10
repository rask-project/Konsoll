#include "Charsets.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace konsoll
{

namespace
{

// DEC Special Graphics (ESC ( 0) for 0x5f..0x7e: line drawing and symbols.
constexpr std::array<char16_t, 32> kDecSpecialGraphics{
    u' ', u'◆', u'▒', u'␉', u'␌', u'␍', u'␊', u'°',
    u'±', u'␤', u'␋', u'┘', u'┐', u'┌', u'└', u'┼',
    u'⎺', u'⎻', u'─', u'⎼', u'⎽', u'├', u'┤', u'┴',
    u'┬', u'│', u'≤', u'≥', u'π', u'≠', u'£', u'·',
};

// National Replacement Character sets: replacements for the 12 positions
// # @ [ \ ] ^ _ ` { | } ~ (0 = unchanged), after xterm's tables.
constexpr std::string_view kNrcPositions = "#@[\\]^_`{|}~";

struct NrcSet {
    std::string_view finals; // designation finals selecting this set
    std::array<char16_t, 12> map;
};

constexpr std::array kNrcSets{
    NrcSet{.finals = "4", // Dutch
           .map = {u'£', u'¾', u'ĳ', u'½', u'|', 0, 0, 0, u'¨', u'ƒ', u'¼', u'´'}},
    NrcSet{.finals = "C5", // Finnish
           .map = {0, 0, u'Ä', u'Ö', u'Å', u'Ü', 0, u'é', u'ä', u'ö', u'å', u'ü'}},
    NrcSet{.finals = "Rf", // French
           .map = {u'£', u'à', u'°', u'ç', u'§', 0, 0, 0, u'é', u'ù', u'è', u'¨'}},
    NrcSet{.finals = "Q9", // French Canadian
           .map = {0, u'à', u'â', u'ç', u'ê', u'î', 0, u'ô', u'é', u'ù', u'è', u'û'}},
    NrcSet{.finals = "K", // German
           .map = {0, u'§', u'Ä', u'Ö', u'Ü', 0, 0, 0, u'ä', u'ö', u'ü', u'ß'}},
    NrcSet{.finals = "Y", // Italian
           .map = {u'£', u'§', u'°', u'ç', u'é', 0, 0, u'ù', u'à', u'ò', u'è', u'ì'}},
    NrcSet{.finals = "E6`", // Norwegian / Danish
           .map = {0, u'Ä', u'Æ', u'Ø', u'Å', u'Ü', 0, u'ä', u'æ', u'ø', u'å', u'ü'}},
    NrcSet{.finals = "P", // Portuguese (ESC ( % 6, mapped to Charset::Portuguese)
           .map = {0, 0, u'Ã', u'Ç', u'Õ', 0, 0, 0, u'ã', u'ç', u'õ', 0}},
    NrcSet{.finals = "Z", // Spanish
           .map = {u'£', u'§', u'¡', u'Ñ', u'¿', 0, 0, 0, u'°', u'ñ', u'ç', 0}},
    NrcSet{.finals = "H7", // Swedish
           .map = {0, u'É', u'Ä', u'Ö', u'Å', u'Ü', 0, u'é', u'ä', u'ö', u'å', u'ü'}},
    NrcSet{.finals = "=", // Swiss
           .map = {u'ù', u'à', u'é', u'ç', u'ê', u'î', u'è', u'ô', u'ä', u'ö', u'ü', u'û'}},
};

// DEC Supplemental Graphics: the upper half of Latin-1, with the DEC
// differences (¤, Œ, Ÿ, œ, ÿ).
unsigned int decSupplemental(unsigned char c)
{
    switch (c) {
    case 0x28:
        return 0xa4;
    case 0x57:
        return 0x152;
    case 0x5d:
        return 0x178;
    case 0x77:
        return 0x153;
    case 0x7d:
        return 0xff;
    default:
        return c + 0x80u;
    }
}

unsigned int nationalReplacement(char charset, unsigned char c)
{
    const auto *const set = std::ranges::find_if(
        kNrcSets, [charset](const NrcSet &nrc) { return nrc.finals.find(charset) != std::string_view::npos; });
    const std::size_t position = kNrcPositions.find(static_cast<char>(c));
    if (set == kNrcSets.end() || position == std::string_view::npos || set->map[position] == 0) {
        return c;
    }
    return set->map[position];
}

} // namespace

char designatedCharset(const QByteArray &intermediates, char final, int &slot)
{
    slot = -1;
    if (intermediates.isEmpty()) {
        return Charset::Ascii;
    }
    constexpr std::string_view k94Slots = "()*+";
    constexpr std::string_view k96Slots = ",-./";
    const char first = intermediates.front();
    const bool percent = intermediates.size() > 1 && intermediates[1] == '%';
    if (const std::size_t index = k94Slots.find(first); index != std::string_view::npos) {
        slot = static_cast<int>(index);
        if (percent) {
            return final == '6' ? Charset::Portuguese : (final == '5' ? Charset::DecSupplemental : Charset::Ascii);
        }
        return final == '<' ? Charset::DecSupplemental : final;
    }
    if (const std::size_t index = k96Slots.find(first); index != std::string_view::npos && index > 0) {
        slot = static_cast<int>(index);
        return final == 'A' ? Charset::Latin1 : Charset::Ascii; // ISO Latin-1 is the 96-set we know
    }
    return Charset::Ascii;
}

unsigned int mapCharsetByte(char charset, unsigned char c, bool nrcMode)
{
    switch (charset) {
    case Charset::Ascii:
        return c;
    case Charset::DecSpecialGraphics:
        return c >= 0x5f && c <= 0x7e ? kDecSpecialGraphics[c - 0x5f] : c;
    case Charset::British:
        return c == '#' ? 0xa3u : c;
    case Charset::Latin1:
        return c + 0x80u; // 0x20..0x7f -> 0xa0..0xff
    case Charset::DecSupplemental:
        return c == 0x20 || c == 0x7f ? c : decSupplemental(c);
    default:
        return nrcMode ? nationalReplacement(charset, c) : c;
    }
}

} // namespace konsoll
