#pragma once

#include <QString>

#include <cstdint>

namespace konsoll
{

// The kind of program a tab shows an icon for.
enum class ProcessKind : std::uint8_t {
    None,
    Remote,
    Editor,
    Monitor,
    Container,
    Repl,
    Database,
    Privileged,
    Pager,
    Git,
    Build,
    Multiplexer
};

// The kind of a foreground job from its executable name ("nvim" -> Editor,
// "ssh" -> Remote). Version suffixes are ignored ("python3.12" is
// "python") and case does not matter ("Python").
ProcessKind processKind(const QString &executableName);

// The name QML uses for a kind (and its icon): "remote", "editor", ...;
// empty for None.
QString processKindName(ProcessKind kind);

} // namespace konsoll
