#include "ProcessKind.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>

namespace konsoll
{

namespace
{

using namespace std::string_view_literals;

constexpr auto kRemote = std::to_array({"et"sv, "mosh"sv, "mosh-client"sv, "sftp"sv, "ssh"sv, "telnet"sv});
constexpr auto kEditor =
    std::to_array({"emacs"sv, "hx"sv, "kak"sv, "micro"sv, "nano"sv, "nvim"sv, "vi"sv, "view"sv, "vim"sv});
constexpr auto kMonitor = std::to_array({"atop"sv, "btm"sv, "btop"sv, "glances"sv, "htop"sv, "top"sv, "watch"sv});
constexpr auto kContainer = std::to_array({"docker"sv, "k9s"sv, "kubectl"sv, "lazydocker"sv, "podman"sv});
constexpr auto kRepl =
    std::to_array({"bun"sv, "deno"sv, "ghci"sv, "iex"sv, "ipython"sv, "irb"sv, "julia"sv, "lua"sv, "node"sv, "python"sv}
    );
constexpr auto kDatabase =
    std::to_array({"mongosh"sv, "mycli"sv, "mysql"sv, "pgcli"sv, "psql"sv, "redis-cli"sv, "sqlite"sv});
constexpr auto kPrivileged = std::to_array({"doas"sv, "su"sv, "sudo"sv});
constexpr auto kPager = std::to_array({"less"sv, "man"sv, "more"sv, "most"sv});
constexpr auto kGit = std::to_array({"git"sv, "gitui"sv, "lazygit"sv, "tig"sv});
constexpr auto kBuild = std::to_array(
    {"cargo"sv,
     "cmake"sv,
     "ctest"sv,
     "go"sv,
     "gradle"sv,
     "make"sv,
     "mvn"sv,
     "ninja"sv,
     "npm"sv,
     "pnpm"sv,
     "swift"sv,
     "xcodebuild"sv,
     "yarn"sv}
);
constexpr auto kMultiplexer = std::to_array({"screen"sv, "tmux"sv, "zellij"sv});
constexpr auto kAgent = std::to_array(
    {"aider"sv,
     "amp"sv,
     "claude"sv,
     "codex"sv,
     "copilot"sv,
     "crush"sv,
     "cursor-agent"sv,
     "gemini"sv,
     "goose"sv,
     "opencode"sv,
     "qwen"sv}
);
constexpr auto kNetwork = std::to_array(
    {"aria2c"sv, "curl"sv, "http"sv, "nc"sv, "ping"sv, "rsync"sv, "scp"sv, "traceroute"sv, "wget"sv, "xh"sv}
);
constexpr auto kArchive =
    std::to_array({"7z"sv, "bzip2"sv, "gunzip"sv, "gzip"sv, "tar"sv, "unrar"sv, "unzip"sv, "xz"sv, "zip"sv, "zstd"sv});
constexpr auto kPackage =
    std::to_array({"apt"sv, "brew"sv, "conda"sv, "gem"sv, "mas"sv, "pip"sv, "pipx"sv, "port"sv, "uv"sv});
constexpr auto kSearch = std::to_array({"ag"sv, "fd"sv, "find"sv, "grep"sv, "locate"sv, "mdfind"sv, "rg"sv});
constexpr auto kWait = std::to_array({"caffeinate"sv, "sleep"sv, "wait"sv});

// Interpreters whose script argument names the program.
constexpr auto kInterpreters = std::to_array({"bun"sv, "node"sv, "perl"sv, "python"sv, "ruby"sv});
// Options after which the interpreter runs inline code, not a script.
constexpr auto kInlineCodeOptions = std::to_array({"-c"sv, "-e"sv, "-p"sv, "--eval"sv, "--print"sv});
// Script extensions dropped from the program name ("codex.js" -> "codex").
constexpr auto kScriptSuffixes = std::to_array({"cjs"sv, "js"sv, "mjs"sv, "pl"sv, "py"sv, "rb"sv, "ts"sv});

// The executables of each kind, indexed by ProcessKind.
constexpr auto kExecutables = std::to_array<std::span<const std::string_view>>(
    {{},
     kRemote,
     kEditor,
     kMonitor,
     kContainer,
     kRepl,
     kDatabase,
     kPrivileged,
     kPager,
     kGit,
     kBuild,
     kMultiplexer,
     kAgent,
     kNetwork,
     kArchive,
     kPackage,
     kSearch,
     kWait}
);

// processKindName(), indexed by ProcessKind.
constexpr auto kKindNames = std::to_array(
    {""sv,
     "remote"sv,
     "editor"sv,
     "monitor"sv,
     "container"sv,
     "repl"sv,
     "database"sv,
     "privileged"sv,
     "pager"sv,
     "git"sv,
     "build"sv,
     "multiplexer"sv,
     "agent"sv,
     "network"sv,
     "archive"sv,
     "package"sv,
     "search"sv,
     "wait"sv}
);

constexpr std::size_t kKindCount = static_cast<std::size_t>(ProcessKind::Wait) + 1;
static_assert(kExecutables.size() == kKindCount && kKindNames.size() == kKindCount);

// `name` lowered and without a version suffix: "Python3.12" -> "python";
// empty when it is only a version ("2.1.3").
QByteArray baseName(const QString &name)
{
    QByteArray lowered = name.toLower().toUtf8();
    const std::string_view view{lowered.constData(), static_cast<std::size_t>(lowered.size())};
    const auto versionStart = view.find_last_not_of("0123456789.");
    lowered.truncate(versionStart == std::string_view::npos ? 0 : static_cast<qsizetype>(versionStart + 1));
    return lowered;
}

std::string_view toView(const QByteArray &bytes)
{
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// The program a script path names: its file name without a script
// extension.
QString scriptName(const QString &path)
{
    const QFileInfo info(path);
    const QByteArray suffix = info.suffix().toLower().toUtf8();
    return std::ranges::contains(kScriptSuffixes, toView(suffix)) ? info.completeBaseName() : info.fileName();
}

} // namespace

ProcessKind processKind(const QString &executableName)
{
    // "python3.12" / "sqlite3" -> base name; "Python" (framework) -> python.
    const QByteArray name = baseName(executableName);
    if (name.isEmpty()) {
        return ProcessKind::None;
    }
    const auto kinds = std::views::iota(std::size_t{1}, kKindCount);
    const auto match = std::ranges::find_if(kinds, [&name](std::size_t kind) {
        return std::ranges::contains(kExecutables.at(kind), toView(name));
    });
    return match == kinds.end() ? ProcessKind::None : static_cast<ProcessKind>(*match);
}

bool isScriptInterpreter(const QString &executableName)
{
    return std::ranges::contains(kInterpreters, toView(baseName(executableName)));
}

QString programName(const QString &executablePath, const QStringList &arguments)
{
    const QFileInfo executable(executablePath);
    const QString name = executable.fileName();
    if (!name.isEmpty() && baseName(name).isEmpty()) {
        // A binary named by its version: ".../claude/versions/2.1.3".
        const QDir directory = executable.dir();
        const QString parent = directory.dirName();
        return parent.compare(QStringLiteral("versions"), Qt::CaseInsensitive) == 0
                   ? QFileInfo(directory.absolutePath()).dir().dirName()
                   : parent;
    }
    if (!isScriptInterpreter(name)) {
        return name;
    }
    // The first argument after the interpreter's options is the script;
    // without one (or with inline code: "python -c ...") the interpreter
    // names the program.
    const auto afterInterpreter = arguments | std::views::drop(1);
    const auto script = std::ranges::find_if(afterInterpreter, [](const QString &argument) {
        const QByteArray option = argument.toUtf8();
        return !argument.startsWith(u'-') || std::ranges::contains(kInlineCodeOptions, toView(option));
    });
    return script == afterInterpreter.end() || script->startsWith(u'-') ? name : scriptName(*script);
}

QString processKindName(ProcessKind kind)
{
    const std::string_view name = kKindNames.at(static_cast<std::size_t>(kind));
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

} // namespace konsoll
