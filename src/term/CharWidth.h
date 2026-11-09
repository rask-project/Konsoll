#pragma once

namespace konsoll
{

// True for code points that occupy two terminal cells: East Asian Wide /
// Fullwidth characters and emoji with default emoji presentation
// (Unicode 15.1), matching what wcwidth-style TUIs (OpenTUI, Ink, ncurses)
// assume when they lay out a row.
bool isWideChar(unsigned int codePoint);

} // namespace konsoll
