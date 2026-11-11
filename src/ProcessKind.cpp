#include "ProcessKind.h"

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
constexpr auto kMonitor = std::to_array({"atop"sv, "btm"sv, "btop"sv, "glances"sv, "htop"sv, "top"sv});
constexpr auto kContainer = std::to_array({"docker"sv, "k9s"sv, "kubectl"sv, "lazydocker"sv, "podman"sv});
constexpr auto kRepl = std::to_array(
    {"bun"sv, "deno"sv, "ghci"sv, "iex"sv, "ipython"sv, "irb"sv, "julia"sv, "lua"sv, "node"sv, "python"sv});
constexpr auto kDatabase =
    std::to_array({"mongosh"sv, "mycli"sv, "mysql"sv, "pgcli"sv, "psql"sv, "redis-cli"sv, "sqlite"sv});
constexpr auto kPrivileged = std::to_array({"doas"sv, "su"sv, "sudo"sv});
constexpr auto kPager = std::to_array({"less"sv, "man"sv, "more"sv, "most"sv});
constexpr auto kGit = std::to_array({"git"sv, "gitui"sv, "lazygit"sv, "tig"sv});
constexpr auto kBuild = std::to_array({"cargo"sv, "cmake"sv, "ctest"sv, "go"sv, "gradle"sv, "make"sv, "mvn"sv, "ninja"sv,
                                       "npm"sv, "pnpm"sv, "swift"sv, "xcodebuild"sv, "yarn"sv});
constexpr auto kMultiplexer = std::to_array({"screen"sv, "tmux"sv, "zellij"sv});

// The executables of each kind, indexed by ProcessKind.
constexpr auto kExecutables = std::to_array<std::span<const std::string_view>>(
    {{}, kRemote, kEditor, kMonitor, kContainer, kRepl, kDatabase, kPrivileged, kPager, kGit, kBuild, kMultiplexer});

// processKindName(), indexed by ProcessKind.
constexpr auto kKindNames = std::to_array({""sv, "remote"sv, "editor"sv, "monitor"sv, "container"sv, "repl"sv,
                                           "database"sv, "privileged"sv, "pager"sv, "git"sv, "build"sv,
                                           "multiplexer"sv});

constexpr std::size_t kKindCount = static_cast<std::size_t>(ProcessKind::Multiplexer) + 1;
static_assert(kExecutables.size() == kKindCount && kKindNames.size() == kKindCount);

} // namespace

ProcessKind processKind(const QString &executableName)
{
    // "python3.12" / "sqlite3" -> base name; "Python" (framework) -> python.
    const QByteArray lowered = executableName.toLower().toUtf8();
    std::string_view name{lowered.constData(), static_cast<std::size_t>(lowered.size())};
    const auto versionStart = name.find_last_not_of("0123456789.");
    name = versionStart == std::string_view::npos ? std::string_view{} : name.substr(0, versionStart + 1);
    if (name.empty()) {
        return ProcessKind::None;
    }
    const auto kinds = std::views::iota(std::size_t{1}, kKindCount);
    const auto match = std::ranges::find_if(
        kinds, [name](std::size_t kind) { return std::ranges::contains(kExecutables.at(kind), name); });
    return match == kinds.end() ? ProcessKind::None : static_cast<ProcessKind>(*match);
}

QString processKindName(ProcessKind kind)
{
    const std::string_view name = kKindNames.at(static_cast<std::size_t>(kind));
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

} // namespace konsoll
