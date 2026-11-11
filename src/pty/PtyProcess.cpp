#include "PtyProcess.h"

#include "ProcessKind.h"

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <libproc.h>
#include <ranges>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <termios.h>
#include <sys/wait.h>
#include <unistd.h>
#include <util.h>

#include <utility>
#include <vector>

namespace
{

struct ShellIntegration {
    QString integrationFile;
    QString startupDir;
    QByteArray originalStartupDir;
    QByteArray originalHome;
    std::vector<QByteArray> shellArgs;
};

void installResource(const QString &resource, const QString &destination)
{
    QFile source(resource);
    if (!source.open(QIODevice::ReadOnly)) {
        return;
    }
    QSaveFile target(destination);
    if (!target.open(QIODevice::WriteOnly) || target.write(source.readAll()) < 0) {
        return;
    }
    target.commit();
}

void prepareZshIntegration(const QString &supportDir, ShellIntegration &integration)
{
    integration.integrationFile = supportDir + QStringLiteral("/konsoll.zsh");
    integration.startupDir = supportDir + QStringLiteral("/zsh-startup");
    const QByteArray configuredZdotdir = qgetenv("ZDOTDIR");
    integration.originalStartupDir = configuredZdotdir.isEmpty() ? qgetenv("HOME") : configuredZdotdir;
    if (integration.originalStartupDir.isEmpty()) {
        integration.originalStartupDir = QDir::homePath().toLocal8Bit();
    }
    QDir().mkpath(integration.startupDir);
    installResource(QStringLiteral(":/shell-integration/konsoll.zsh"), integration.integrationFile);
    const QByteArray sourceHelper =
        "typeset -g _KONSOLL_WRAPPER_ZDOTDIR=\"$ZDOTDIR\"\n"
        "typeset -g _KONSOLL_USER_ZDOTDIR=\"${KONSOLL_USER_ZDOTDIR:-$HOME}\"\n"
        "function _konsoll_source_startup_file() {\n"
        "  local file=\"$1\"\n"
        "  local startup_path=\"$_KONSOLL_USER_ZDOTDIR/$file\"\n"
        "  if [[ -r \"$startup_path\" ]]; then\n"
        "    ZDOTDIR=\"$_KONSOLL_USER_ZDOTDIR\"\n"
        "    source \"$startup_path\"\n"
        "    _KONSOLL_USER_ZDOTDIR=\"${ZDOTDIR:-$_KONSOLL_USER_ZDOTDIR}\"\n"
        "  fi\n"
        "  ZDOTDIR=\"$_KONSOLL_WRAPPER_ZDOTDIR\"\n"
        "}\n";
    const std::array<std::pair<QString, QByteArray>, 3> startupFiles{{
        {QStringLiteral(".zshenv"),
         sourceHelper + "_konsoll_source_startup_file .zshenv\n"},
        {QStringLiteral(".zprofile"), "_konsoll_source_startup_file .zprofile\n"},
        {QStringLiteral(".zshrc"),
         "_konsoll_source_startup_file .zshrc\n"
         "if [[ \"$HISTFILE\" == \"$_KONSOLL_WRAPPER_ZDOTDIR/.zsh_history\" ]]; then\n"
         "  HISTFILE=\"$_KONSOLL_USER_ZDOTDIR/.zsh_history\"\n"
         "  [[ -r \"$HISTFILE\" ]] && fc -R \"$HISTFILE\"\n"
         "fi\n"
         "source \"$KONSOLL_SHELL_INTEGRATION\"\n"
         "ZDOTDIR=\"$_KONSOLL_USER_ZDOTDIR\"\n"},
    }};
    for (const auto &[name, contents] : startupFiles) {
        QSaveFile file(integration.startupDir + QLatin1Char('/') + name);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(contents);
            file.commit();
        }
    }
}

void prepareBashIntegration(const QString &supportDir, ShellIntegration &integration)
{
    integration.integrationFile = supportDir + QStringLiteral("/konsoll.bash");
    integration.startupDir = supportDir + QStringLiteral("/bash-home");
    integration.originalHome = qgetenv("HOME");
    if (integration.originalHome.isEmpty()) {
        integration.originalHome = QDir::homePath().toLocal8Bit();
    }
    QDir().mkpath(integration.startupDir);
    installResource(QStringLiteral(":/shell-integration/konsoll.bash"), integration.integrationFile);
    QSaveFile profile(integration.startupDir + QStringLiteral("/.bash_profile"));
    if (profile.open(QIODevice::WriteOnly)) {
        profile.write(
            "export HOME=\"$KONSOLL_ORIGINAL_HOME\"\n"
            "for _konsoll_profile in .bash_profile .bash_login .profile; do\n"
            "  if [[ -r \"$HOME/$_konsoll_profile\" ]]; then\n"
            "    source \"$HOME/$_konsoll_profile\"\n"
            "    break\n"
            "  fi\n"
            "done\n"
            "source \"$KONSOLL_SHELL_INTEGRATION\"\n");
        profile.commit();
    }
}

void prepareFishIntegration(const QString &supportDir, ShellIntegration &integration)
{
    integration.integrationFile = supportDir + QStringLiteral("/konsoll.fish");
    installResource(QStringLiteral(":/shell-integration/konsoll.fish"), integration.integrationFile);
    integration.shellArgs = {"--init-command", "source $KONSOLL_FISH_INTEGRATION"};
}

std::vector<QByteArray>
buildPtyEnvironment(const ShellIntegration &integration, bool isZsh, bool isBash, bool isFish, int cols, int rows,
                     const QString &remoteHostToken)
{
    // Build the full environment in the parent (before fork) so the child runs
    // only async-signal-safe calls between fork() and exec(). setenv()
    // allocates and would deadlock on a heap lock inherited from another
    // thread once the app is multithreaded; execvpe() sets the environment
    // atomically at exec time instead.
    const QProcessEnvironment currentEnvironment = QProcessEnvironment::systemEnvironment();
    std::vector<QByteArray> envp;
    const QStringList environmentKeys = currentEnvironment.keys();
    for (const QString &key : environmentKeys) {
        envp.push_back(key.toLocal8Bit() + '=' + currentEnvironment.value(key).toLocal8Bit());
    }
    const auto setEnvironmentVariable = [&envp](const char *key, const QByteArray &value) {
        const QByteArray keyBytes = key;
        const QByteArray prefix = keyBytes + '=';
        std::vector<QByteArray> remaining;
        remaining.reserve(envp.size());
        for (const auto &entry : envp) {
            if (!entry.startsWith(prefix)) {
                remaining.push_back(entry);
            }
        }
        envp = std::move(remaining);
        envp.push_back(keyBytes + '=' + value);
    };
    setEnvironmentVariable("TERM", "xterm-256color");
    setEnvironmentVariable("COLUMNS", QByteArray::number(cols));
    setEnvironmentVariable("LINES", QByteArray::number(rows));
    setEnvironmentVariable("PATH", "/usr/bin:/bin:/usr/sbin:/sbin:" + qgetenv("PATH"));
    if (!remoteHostToken.isEmpty()) {
        setEnvironmentVariable("KONSOLL_REMOTE_HOST_TOKEN", remoteHostToken.toUtf8());
    }
    if (!integration.integrationFile.isEmpty()) {
        const QByteArray integrationPath = integration.integrationFile.toLocal8Bit();
        const QByteArray startupPath = integration.startupDir.toLocal8Bit();
        setEnvironmentVariable("KONSOLL_SHELL_INTEGRATION", integrationPath);
        if (isZsh) {
            setEnvironmentVariable("KONSOLL_USER_ZDOTDIR", integration.originalStartupDir);
            setEnvironmentVariable("ZDOTDIR", startupPath);
        } else if (isBash) {
            setEnvironmentVariable("KONSOLL_ORIGINAL_HOME", integration.originalHome);
            setEnvironmentVariable("HOME", startupPath);
        } else if (isFish) {
            setEnvironmentVariable("KONSOLL_FISH_INTEGRATION", integrationPath);
        }
    }
    return envp;
}

void execShellInChild(const QByteArray &shellBytes, const std::vector<char *> &argv, std::vector<QByteArray> &envp,
                       const QByteArray &initialDirectory, const QByteArray &homePath)
{
    // Child: exec the shell as a login shell (argv[0] = "-<name>").
    // forkpty leaves software flow control enabled by default. Disable
    // it on the slave before the shell starts, so Ctrl+Q reaches Neovim
    // instead of being consumed by the tty line discipline.
    struct termios terminalSettings {};
    if (::tcgetattr(STDIN_FILENO, &terminalSettings) == 0) {
        terminalSettings.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF));
        ::tcsetattr(STDIN_FILENO, TCSANOW, &terminalSettings);
    }
    // GUI apps can inherit `/` (or another launcher's working directory).
    // Start in the inherited session directory, falling back to the user's home.
    if (!initialDirectory.isEmpty() && ::chdir(initialDirectory.constData()) < 0 && !homePath.isEmpty()) {
        ::chdir(homePath.constData());
    }
    // The environment was built in the parent; pass it through execve so
    // the child runs only async-signal-safe calls (tcgetattr, tcsetattr,
    // chdir, execve) between fork() and exec(). The shell path is always
    // absolute (from detectDefaultShell), so execve needs no PATH search.
    std::vector<char *> envPtrs;
    envPtrs.reserve(envp.size() + 1);
    std::ranges::transform(envp, std::back_inserter(envPtrs), [](QByteArray &entry) { return entry.data(); });
    envPtrs.push_back(nullptr);
    ::execve(shellBytes.constData(), argv.data(), envPtrs.data());
    ::_exit(127);
}

// The arguments `pid` was started with (argv, including argv[0]); empty
// when they cannot be read. KERN_PROCARGS2 holds argc, the executable path,
// NUL padding, then argv and the environment, all NUL-terminated.
QStringList processArguments(pid_t pid)
{
    int argumentMax = 0;
    std::size_t length = sizeof(argumentMax);
    std::array<int, 2> argMaxMib{CTL_KERN, KERN_ARGMAX};
    if (::sysctl(argMaxMib.data(), argMaxMib.size(), &argumentMax, &length, nullptr, 0) != 0 || argumentMax <= 0) {
        return {};
    }
    QByteArray buffer(argumentMax, '\0');
    std::size_t size = buffer.size();
    std::array<int, 3> mib{CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
    int argc = 0;
    if (::sysctl(mib.data(), mib.size(), buffer.data(), &size, nullptr, 0) != 0 || size <= sizeof(argc)) {
        return {};
    }
    std::memcpy(&argc, buffer.constData(), sizeof(argc));
    const QList<QByteArray> fields = buffer.first(static_cast<qsizetype>(size)).sliced(sizeof(argc)).split('\0');
    QStringList arguments;
    std::ranges::transform(fields | std::views::drop(1) |
                               std::views::drop_while([](const QByteArray &field) { return field.isEmpty(); }) |
                               std::views::take(std::max(argc, 0)),
                           std::back_inserter(arguments), [](const QByteArray &field) { return QString::fromUtf8(field); });
    return arguments;
}

} // namespace

namespace konsoll
{

PtyProcess::PtyProcess(QObject *parent) : QObject(parent) {}

PtyProcess::~PtyProcess()
{
    closeMaster();
    if (m_pid > 0) {
        const pid_t pid = static_cast<pid_t>(m_pid);
        if (::kill(-pid, SIGHUP) < 0) {
            ::kill(pid, SIGHUP);
        }

        int status = 0;
        pid_t result = ::waitpid(pid, &status, WNOHANG);
        while (result < 0 && errno == EINTR) {
            result = ::waitpid(pid, &status, WNOHANG);
        }

        if (result == 0) {
            if (::kill(-pid, SIGKILL) < 0) {
                ::kill(pid, SIGKILL);
            }
            result = ::waitpid(pid, &status, 0);
            while (result < 0 && errno == EINTR) {
                result = ::waitpid(pid, &status, 0);
            }
        }
        m_pid = -1;
    }
}

bool PtyProcess::start(const QString &shellPath, int cols, int rows, const QString &workingDirectory,
                       const QString &remoteHostToken)
{
    const QString shellName = QFileInfo(shellPath).fileName();
    const bool isZsh = shellName == QStringLiteral("zsh");
    const bool isBash = shellName == QStringLiteral("bash");
    const bool isFish = shellName == QStringLiteral("fish");
    const QString supportDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                               QStringLiteral("/shell-integration");
    const bool supportDirReady = QDir().mkpath(supportDir);
    ShellIntegration integration;
    if (supportDirReady) {
        if (isZsh) {
            prepareZshIntegration(supportDir, integration);
        } else if (isBash) {
            prepareBashIntegration(supportDir, integration);
        } else if (isFish) {
            prepareFishIntegration(supportDir, integration);
        }
    }

    struct winsize ws;
    ws.ws_col = static_cast<short>(cols);
    ws.ws_row = static_cast<short>(rows);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    const QByteArray shellBytes = shellPath.toLocal8Bit();
    QByteArray argv0 = "-" + QFileInfo(shellPath).baseName().toLocal8Bit();
    QByteArray homePath = qgetenv("HOME");
    if (homePath.isEmpty()) {
        homePath = QDir::homePath().toLocal8Bit();
    }
    QByteArray initialDirectory = workingDirectory.toLocal8Bit();
    if (initialDirectory.isEmpty() || !QDir(workingDirectory).exists()) {
        initialDirectory = homePath;
    }
    std::vector<char *> argv;
    argv.reserve(integration.shellArgs.size() + 2);
    argv.push_back(argv0.data());
    std::ranges::transform(integration.shellArgs, std::back_inserter(argv),
                           [](QByteArray &arg) { return arg.data(); });
    argv.push_back(nullptr);

    std::vector<QByteArray> envp =
        buildPtyEnvironment(integration, isZsh, isBash, isFish, cols, rows, remoteHostToken);

    int master = 0;
    // Let forkpty configure the slave's normal terminal defaults. Passing a
    // zero-initialized termios here disables canonical input and echo.
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        execShellInChild(shellBytes, argv, envp, initialDirectory, homePath);
    }

    // Parent.
    m_masterFd = master;
    m_pid = pid;

    // Non-blocking reads so the notifier never blocks the GUI thread.
    const int fl = fcntl(m_masterFd, F_GETFL, 0);
    const unsigned int newFl = static_cast<unsigned int>(fl) | static_cast<unsigned int>(O_NONBLOCK);
    fcntl(m_masterFd, F_SETFL, static_cast<int>(newFl));

    m_notifier = std::make_unique<QSocketNotifier>(m_masterFd, QSocketNotifier::Read, this);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &PtyProcess::onReadable);
    return true;
}

void PtyProcess::write(const QByteArray &data) const
{
    if (m_masterFd < 0 || data.isEmpty()) {
        return;
    }

    int off = 0;
    const auto len = data.size();
    while (off < len) {
        const ssize_t n = ::write(m_masterFd, data.constData() + off, static_cast<size_t>(len - off));
        if (n > 0) {
            off += static_cast<int>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // Master buffer full; drop the remainder (MVP).
        }
        break;
    }
}

void PtyProcess::resize(int cols, int rows) const // NOLINT: cols/rows are inherently swappable
{
    if (m_masterFd < 0) {
        return;
    }
    struct winsize ws;
    ws.ws_col = static_cast<short>(cols);
    ws.ws_row = static_cast<short>(rows);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(m_masterFd, TIOCSWINSZ, &ws);
}

void PtyProcess::terminate() const
{
    if (m_pid > 0) {
        const pid_t pid = static_cast<pid_t>(m_pid);
        if (::kill(-pid, SIGHUP) < 0) {
            ::kill(pid, SIGHUP);
        }
    }
}

void PtyProcess::kill() const
{
    if (m_pid > 0) {
        const pid_t pid = static_cast<pid_t>(m_pid);
        if (::kill(-pid, SIGKILL) < 0) {
            ::kill(pid, SIGKILL);
        }
    }
}

int PtyProcess::waitForExit()
{
    if (m_pid <= 0) {
        return -1;
    }
    int status = 0;
    const pid_t pid = static_cast<pid_t>(m_pid);
    pid_t result = ::waitpid(pid, &status, 0);
    while (result < 0 && errno == EINTR) {
        result = ::waitpid(pid, &status, 0);
    }
    m_pid = -1;
    return (result > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool PtyProcess::tryReap()
{
    if (m_pid <= 0) {
        return true;
    }
    int status = 0;
    const pid_t pid = static_cast<pid_t>(m_pid);
    pid_t result = ::waitpid(pid, &status, WNOHANG);
    while (result < 0 && errno == EINTR) {
        result = ::waitpid(pid, &status, WNOHANG);
    }
    if (result > 0) {
        m_pid = -1;
        return true;
    }
    return false;
}

void PtyProcess::close()
{
    closeMaster();
}

void PtyProcess::pauseReading()
{
    if (m_notifier) {
        m_notifier->setEnabled(false);
    }
}

void PtyProcess::resumeReading()
{
    if (m_notifier) {
        m_notifier->setEnabled(true);
    }
}

pid_t PtyProcess::foregroundGroup() const
{
    if (m_masterFd < 0 || m_pid <= 0) {
        return -1;
    }
    const pid_t foregroundGroup = ::tcgetpgrp(m_masterFd);
    const pid_t shellGroup = ::getpgid(static_cast<pid_t>(m_pid));
    const pid_t expectedShellGroup = shellGroup > 0 ? shellGroup : static_cast<pid_t>(m_pid);
    return foregroundGroup > 0 && foregroundGroup != expectedShellGroup ? foregroundGroup : -1;
}

QString PtyProcess::foregroundProcessName() const
{
    // The group leader (its pid is the group id) is the job the shell
    // started: the first command of a pipeline, or sudo / ssh themselves.
    const pid_t group = foregroundGroup();
    if (group <= 0) {
        return {};
    }
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    if (::proc_pidpath(group, path.data(), static_cast<std::uint32_t>(path.size())) > 0) {
        // Scripts run by an interpreter are named by the script ("node
        // .../bin/codex" -> "codex"), so their arguments are read too.
        const QString executable = QString::fromUtf8(path.data());
        return programName(executable, isScriptInterpreter(QFileInfo(executable).fileName())
                                           ? processArguments(group)
                                           : QStringList{});
    }
    // The leader may have exited while the rest of the group runs on; any
    // job still counts as a foreground process.
    std::array<char, (2 * MAXCOMLEN) + 1> name{};
    if (::proc_name(group, name.data(), static_cast<std::uint32_t>(name.size())) > 0) {
        return QString::fromUtf8(name.data());
    }
    return QStringLiteral("?");
}

void PtyProcess::onReadable()
{
    std::array<char, 8192> buf;
    bool eof = false;
    for (;;) {
        const ssize_t n = ::read(m_masterFd, buf.data(), buf.size());
        if (n > 0) {
            emit dataReceived(QByteArray(buf.data(), static_cast<int>(n)));
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // Drained; the notifier fires again on new data.
            }
            eof = true;
            break;
        }
    }

    if (eof) {
        // The child's write side is closed. Close the descriptor and let the
        // worker reap the process (eofReached), so no blocking waitpid runs
        // in the read path.
        closeMaster();
        emit eofReached();
    }
}

void PtyProcess::closeMaster()
{
    if (m_notifier) {
        m_notifier->setEnabled(false);
    }
    if (m_masterFd >= 0) {
        ::close(m_masterFd);
        m_masterFd = -1;
    }
}

} // namespace konsoll
