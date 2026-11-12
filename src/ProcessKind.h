#pragma once

#include <QString>
#include <QStringList>

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
    Multiplexer,
    Agent,
    Network,
    Archive,
    Package,
    Search,
    Wait
};

// The kind of a foreground job from its executable name ("nvim" -> Editor,
// "ssh" -> Remote). Version suffixes are ignored ("python3.12" is
// "python") and case does not matter ("Python").
ProcessKind processKind(const QString &executableName);

// Whether `executableName` runs scripts ("node", "python3.12"), so the
// script it runs names the program better than the interpreter.
bool isScriptInterpreter(const QString &executableName);

// The program a foreground job runs, from its executable's path and its
// arguments (argv, including argv[0]): the executable's file name, the
// install directory of a binary named only by its version
// (".../claude/versions/2.1.3" -> "claude"), and the script an interpreter
// runs ("node .../bin/codex" -> "codex", "python .../aider" -> "aider").
QString programName(const QString &executablePath, const QStringList &arguments = {});

// The name QML uses for a kind (and its icon): "remote", "editor", ...;
// empty for None (a job of no known kind shows a generic running icon).
QString processKindName(ProcessKind kind);

} // namespace konsoll
