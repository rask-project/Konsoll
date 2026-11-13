#include "SshIntegration.h"

#include "AgentIntegrations.h"
#include "ShellStartup.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>

#include <algorithm>
#include <array>
#include <utility>

namespace konsoll
{

namespace
{

constexpr QLatin1StringView kEnabledKey("AgentIntegrations/sshIntegration");
constexpr QLatin1StringView kExcludedKey("AgentIntegrations/sshExcludedHosts");

QByteArray resource(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

// A file the bootstrap writes into its directory: a here-document (quoted,
// so nothing in it expands; none of the files has the delimiter's line).
QByteArray hereDocument(const QString &name, QByteArray contents)
{
    if (!contents.endsWith('\n')) {
        contents += '\n';
    }
    return "    cat >\"$_konsoll_dir/" + name.toUtf8() + "\" <<'KONSOLL_EOF'\n" + contents + "KONSOLL_EOF\n";
}

std::expected<void, QString> writeFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
        return std::unexpected(QObject::tr("Could not write %1").arg(path));
    }
    return {};
}

} // namespace

QByteArray sshBootstrapScript()
{
    const auto zshFiles = zshStartupFiles();
    std::array<std::pair<QString, QByteArray>, 11> files{{
        {QStringLiteral("konsoll.zsh"), resource(QStringLiteral(":/shell-integration/konsoll.zsh"))},
        {QStringLiteral("konsoll.bash"), resource(QStringLiteral(":/shell-integration/konsoll.bash"))},
        {QStringLiteral("konsoll-remote.sh"), resource(QStringLiteral(":/shell-integration/konsoll-remote.sh"))},
        {QStringLiteral("konsoll-agent-hook"), resource(QStringLiteral(":/shell-integration/konsoll-remote-hook"))},
        {QStringLiteral("claude-settings.json"),
         QJsonDocument(agentHooksSettings("claude", claudeHookSpecs())).toJson(QJsonDocument::Compact)},
        {QStringLiteral("qwen-settings.json"),
         QJsonDocument(agentHooksSettings("qwen", qwenHookSpecs())).toJson(QJsonDocument::Compact)},
        {QStringLiteral("codex-hooks.toml"), codexHooksToml()},
        {QStringLiteral("zsh/") + zshFiles[0].first, zshFiles[0].second},
        {QStringLiteral("zsh/") + zshFiles[1].first, zshFiles[1].second},
        {QStringLiteral("zsh/") + zshFiles[2].first, zshFiles[2].second},
        {QStringLiteral("bash-home/.bash_profile"), bashProfile()},
    }};
    QByteArray documents;
    std::ranges::for_each(files, [&documents](const auto &file) {
        documents += hereDocument(file.first, file.second);
    });
    // The directory is the shell's: its pid (exec keeps this one) says
    // whether a directory found later is still in use.
    // sshd shows the message of the day and the last login only without a
    // remote command: shown here as it would (pam_motd's /run/motd.dynamic
    // and /etc/motd, then the login before this one from wtmp in ctime's
    // English form), unless ~/.hushlogin asks for quiet.
    return QByteArrayLiteral(
               "_konsoll_login_messages() {\n"
               "    [ -e \"$HOME/.hushlogin\" ] && return 0\n"
               "    for _konsoll_motd in /run/motd.dynamic /etc/motd; do\n"
               "        [ -r \"$_konsoll_motd\" ] && cat \"$_konsoll_motd\"\n"
               "    done\n"
               "    _konsoll_last=$(last -w -n 2 --time-format iso \"${USER:-$(id -un)}\" 2>/dev/null |\n"
               "        awk 'NR == 2 { print $4 \" \" $3 }')\n"
               "    _konsoll_when=$(LC_ALL=C date -d \"${_konsoll_last%% *}\" '+%a %b %e %H:%M:%S %Y' 2>/dev/null) &&\n"
               "        [ -n \"${_konsoll_last#* }\" ] && printf 'Last login: %s from %s\\n' \"$_konsoll_when\" "
               "\"${_konsoll_last#* }\"\n"
               "    return 0\n"
               "}\n"
               "_konsoll_login_messages\n"
               "_konsoll_bootstrap() {\n"
               "    [ -n \"$1\" ] || return 1\n"
               "    _konsoll_shell=${SHELL:-}\n"
               "    case ${_konsoll_shell##*/} in\n"
               "    zsh | bash) ;;\n"
               "    *) return 1 ;;\n"
               "    esac\n"
               "    _konsoll_base=${XDG_RUNTIME_DIR:-}\n"
               "    if [ -z \"$_konsoll_base\" ] || [ ! -d \"$_konsoll_base\" ] || [ ! -w \"$_konsoll_base\" ]; then\n"
               "        _konsoll_base=${TMPDIR:-/tmp}\n"
               "    fi\n"
               "    _konsoll_base=${_konsoll_base%/}\n"
               "    umask 077\n"
               "    for _konsoll_old in \"$_konsoll_base\"/konsoll.*; do\n"
               "        [ -f \"$_konsoll_old/pid\" ] || continue\n"
               "        kill -0 \"$(cat \"$_konsoll_old/pid\" 2>/dev/null)\" 2>/dev/null || rm -rf \"$_konsoll_old\" "
               "2>/dev/null\n"
               "    done\n"
               "    _konsoll_dir=$(mktemp -d \"$_konsoll_base/konsoll.XXXXXXXX\" 2>/dev/null) || return 1\n"
               "    echo \"$$\" >\"$_konsoll_dir/pid\"\n"
               "    mkdir \"$_konsoll_dir/zsh\" \"$_konsoll_dir/bash-home\" || return 1\n"
           ) +
           documents +
           QByteArrayLiteral(
               "    chmod 700 \"$_konsoll_dir/konsoll-agent-hook\"\n"
               "    KONSOLL_TTY=$(tty 2>/dev/null)\n"
               "    case $KONSOLL_TTY in /dev/*) ;; *) KONSOLL_TTY='' ;; esac\n"
               "    KONSOLL_REMOTE=1\n"
               "    KONSOLL_REMOTE_DIR=$_konsoll_dir\n"
               "    KONSOLL_REMOTE_AGENT_TOKEN=$1\n"
               "    KONSOLL_AGENT_HOOK=$_konsoll_dir/konsoll-agent-hook\n"
               "    KONSOLL_SHELL_INTEGRATION=$_konsoll_dir/konsoll.${_konsoll_shell##*/}\n"
               "    export KONSOLL_TTY KONSOLL_REMOTE KONSOLL_REMOTE_DIR KONSOLL_REMOTE_AGENT_TOKEN KONSOLL_AGENT_HOOK "
               "KONSOLL_SHELL_INTEGRATION\n"
               "    unset KONSOLL_SESSION_TOKEN KONSOLL_AGENT_SOCKET KONSOLL_REMOTE_HOST_TOKEN\n"
               "    if [ \"${_konsoll_shell##*/}\" = zsh ]; then\n"
               "        KONSOLL_USER_ZDOTDIR=${KONSOLL_USER_ZDOTDIR:-${ZDOTDIR:-$HOME}}\n"
               "        ZDOTDIR=$_konsoll_dir/zsh\n"
               "        export KONSOLL_USER_ZDOTDIR ZDOTDIR\n"
               "    else\n"
               "        KONSOLL_ORIGINAL_HOME=$HOME\n"
               "        HOME=$_konsoll_dir/bash-home\n"
               "        export KONSOLL_ORIGINAL_HOME HOME\n"
               "    fi\n"
               "    exec \"$_konsoll_shell\" -l\n"
               "}\n"
               "_konsoll_bootstrap \"$@\"\n"
           );
}

QByteArray sshRemoteCommand()
{
    // Single-quoted for the remote login shell (zsh, bash, fish, ...);
    // base64 has no quote.
    return "exec /bin/sh -c 'eval \"$(printf %s " + sshBootstrapScript().toBase64() +
           " | base64 -d 2>/dev/null)\"; exec \"${SHELL:-/bin/sh}\" -l' konsoll";
}

bool sshIntegrationEnabled()
{
    return QSettings().value(kEnabledKey, true).toBool();
}

void setSshIntegrationEnabled(bool enabled)
{
    QSettings().setValue(kEnabledKey, enabled);
}

QStringList sshExcludedHosts()
{
    return QSettings().value(kExcludedKey).toStringList();
}

void setSshExcludedHosts(const QStringList &hosts)
{
    QSettings().setValue(kExcludedKey, hosts);
}

std::expected<void, QString> writeSshIntegration(const QString &directory)
{
    if (!QDir().mkpath(directory)) {
        return std::unexpected(QObject::tr("Could not create %1").arg(directory));
    }
    const QString command = directory + QStringLiteral("/ssh-remote-command");
    if (!sshIntegrationEnabled()) {
        QFile::remove(command);
    } else if (auto written = writeFile(command, sshRemoteCommand()); !written) {
        return written;
    }
    const QStringList hosts = sshExcludedHosts();
    return writeFile(
        directory + QStringLiteral("/ssh-excluded-hosts"),
        hosts.isEmpty() ? QByteArray{} : (hosts.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8()
    );
}

} // namespace konsoll
