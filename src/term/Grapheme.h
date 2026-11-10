#pragma once

#include <QString>

#include <string>

namespace konsoll
{

// Grapheme clusters (mode 2027): a cell's `ch` at or above kClusterBase is
// the id of a cluster of several code points (an emoji ZWJ sequence, a flag,
// a letter with combining marks) interned in a process-wide table.
inline constexpr unsigned int kClusterBase = 0x110000u;

// Interns `cluster` and returns its cell value; `width` is 1 or 2 cells.
unsigned int internCluster(const std::u32string &cluster, int width);
// The code points of a cell value (one, or the cluster's).
std::u32string cellCodePoints(unsigned int ch);
QString cellText(unsigned int ch);
// The width (1 or 2) a cell value takes: a cluster's own, or 0 when `ch` is
// a plain code point (callers use their own width rules for those).
int clusterWidth(unsigned int ch);

// UAX #29 (the cases terminals meet): whether `next` continues the cluster
// `previous` (combining marks, ZWJ + pictographic, variation selectors,
// emoji modifiers, tags, regional indicator pairs, Hangul jamo).
bool extendsCluster(const std::u32string &previous, char32_t next);
// The width of a cluster: 2 with emoji presentation (VS16, a modifier, a
// ZWJ sequence, a flag) or a wide base, else 1.
int graphemeWidth(const std::u32string &cluster);

} // namespace konsoll
