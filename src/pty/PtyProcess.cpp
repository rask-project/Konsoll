#include "PtyProcess.h"

#include "AgentStatus.h"
#include "ProcessKind.h"
#include "ShellFunctions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
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
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <termios.h>
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
    const QByteArray sourceHelper = "typeset -g _KONSOLL_WRAPPER_ZDOTDIR=\"$ZDOTDIR\"\n"
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
        {QStringLiteral(".zshenv"), sourceHelper + "_konsoll_source_startup_file .zshenv\n"},
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
        profile.write("export HOME=\"$KONSOLL_ORIGINAL_HOME\"\n"
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

std::vector<QByteArray> buildPtyEnvironment(
    const ShellIntegration &integration,
    bool isZsh,
    bool isBash,
    bool isFish,
    int cols,
    int rows,
    const QString &remoteHostToken
)
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
        // AI agent hooks report their state with it (AgentStatus.h).
        setEnvironmentVariable("KONSOLL_SESSION_TOKEN", remoteHostToken.toUtf8());
        setEnvironmentVariable("KONSOLL_AGENT_SOCKET", konsoll::agentStatusSocketPath().toLocal8Bit());
        setEnvironmentVariable("KONSOLL_AGENT_HOOK", konsoll::agentHookPath().toLocal8Bit());
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

// The highest descriptor number the child closes before exec, read in the
// parent (getrlimit is not async-signal-safe). Capped so a huge or
// unlimited soft limit does not cost a long close() loop per launch.
int inheritedDescriptorLimit()
{
    constexpr rlim_t kMaxDescriptors = 65536;
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY) {
        return static_cast<int>(kMaxDescriptors);
    }
    return static_cast<int>(std::min(limit.rlim_cur, kMaxDescriptors));
}

void execShellInChild(
    const QByteArray &shellBytes,
    const std::vector<char *> &argv,
    std::vector<QByteArray> &envp,
    const QByteArray &initialDirectory,
    const QByteArray &homePath,
    int descriptorLimit
)
{
    // Keep only the slave on 0-2. Everything else Konsoll has open — above
    // all the other sessions' PTY masters — would otherwise leak into this
    // shell and its jobs: a leaked master keeps that terminal from hanging
    // up when its tab closes, so its shell blocks in exit and never reaps.
    // close() is async-signal-safe.
    std::ranges::for_each(
        std::views::iota(STDERR_FILENO + 1, std::max(descriptorLimit, STDERR_FILENO + 1)),
        [](int fd) { ::close(fd); }
    );
    // Child: exec the shell as a login shell (argv[0] = "-<name>").
    // forkpty leaves software flow control enabled by default. Disable
    // it on the slave before the shell starts, so Ctrl+Q reaches Neovim
    // instead of being consumed by the tty line discipline.
    struct termios terminalSettings{};
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
    // the child runs only async-signal-safe calls (close, tcgetattr,
    // tcsetattr, chdir, execve) between fork() and exec(). The shell path is always
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
    std::ranges::transform(
        fields | std::views::drop(1) | std::views::drop_while([](const QByteArray &field) { return field.isEmpty(); }) |
            std::views::take(std::max(argc, 0)),
        std::back_inserter(arguments),
        [](const QByteArray &field) { return QString::fromUtf8(field); }
    );
    return arguments;
}

// The exit code waitpid reported, -1 when the process did not exit normally.
int exitCodeOf(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Non-blocking waitpid that retries on EINTR. Returns the pid once reaped
// (or -1 when it is not our child any more), 0 while it still runs.
pid_t reapWithoutBlocking(pid_t pid, int &status)
{
    pid_t result = ::waitpid(pid, &status, WNOHANG);
    while (result < 0 && errno == EINTR) {
        result = ::waitpid(pid, &status, WNOHANG);
    }
    return result;
}

// Every process group with a process in session `session` (the shell is
// the session leader, so its pid is the session id). Empty when the
// session has no processes left or cannot be read. macOS answers
// KERN_PROC_SESSION with ENOENT, even for a live session, and kinfo_proc
// carries no session id: list the user's processes and ask getsid().
std::vector<pid_t> sessionProcessGroups(pid_t session)
{
    std::array<int, 4> mib{CTL_KERN, KERN_PROC, KERN_PROC_UID, static_cast<int>(::getuid())};
    std::size_t size = 0;
    if (::sysctl(mib.data(), mib.size(), nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return {};
    }
    // Room for processes started between the two calls.
    std::vector<kinfo_proc> processes((size / sizeof(kinfo_proc)) + 8);
    size = processes.size() * sizeof(kinfo_proc);
    if (::sysctl(mib.data(), mib.size(), processes.data(), &size, nullptr, 0) != 0) {
        return {};
    }
    processes.resize(size / sizeof(kinfo_proc));
    std::erase_if(processes, [session](const kinfo_proc &process) {
        return ::getsid(process.kp_proc.p_pid) != session;
    });
    const pid_t ownGroup = ::getpgrp();
    std::vector<pid_t> groups;
    std::ranges::transform(processes, std::back_inserter(groups), [](const kinfo_proc &process) {
        return process.kp_eproc.e_pgid;
    });
    std::erase_if(groups, [ownGroup](pid_t group) { return group <= 1 || group == ownGroup; });
    std::ranges::sort(groups);
    const auto duplicates = std::ranges::unique(groups);
    groups.erase(duplicates.begin(), duplicates.end());
    return groups;
}

} // namespace

namespace konsoll
{

PtyProcess::PtyProcess(QObject *parent) : QObject(parent) {}

PtyProcess::~PtyProcess()
{
    // Normally the worker's bounded shutdown has signalled the whole
    // session and reaped the child already. Otherwise SIGKILL the shell
    // (its group and itself; no allocation here, so not the session walk)
    // without waiting: a process stuck in the kernel must not block the
    // thread destroying this object.
    if (m_pid > 0) {
        const auto shell = static_cast<pid_t>(m_pid);
        ::kill(-shell, SIGKILL);
        ::kill(shell, SIGKILL);
    }
    closeMaster();
    if (m_pid > 0) {
        int status = 0;
        if (reapWithoutBlocking(static_cast<pid_t>(m_pid), status) == 0) {
            qWarning("Konsoll: shell %d did not exit after SIGKILL; leaving it unreaped", m_pid);
        }
        m_pid = -1;
    }
}

bool PtyProcess::start(
    const QString &shellPath,
    int cols,
    int rows,
    const QString &workingDirectory,
    const QString &remoteHostToken,
    const QString &command
)
{
    const QString shellName = QFileInfo(shellPath).fileName();
    const bool isZsh = shellName == QStringLiteral("zsh");
    const bool isBash = shellName == QStringLiteral("bash");
    const bool isFish = shellName == QStringLiteral("fish");
    const QString supportDir = shellIntegrationDirectory();
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
    if (!command.isEmpty()) {
        // Interactive, so the user's rc files set PATH, aliases and functions
        // up as they would at a prompt.
        integration.shellArgs.emplace_back("-i");
        integration.shellArgs.emplace_back("-c");
        integration.shellArgs.push_back(command.toUtf8());
    }
    std::vector<char *> argv;
    argv.reserve(integration.shellArgs.size() + 2);
    argv.push_back(argv0.data());
    std::ranges::transform(integration.shellArgs, std::back_inserter(argv), [](QByteArray &arg) { return arg.data(); });
    argv.push_back(nullptr);

    std::vector<QByteArray> envp = buildPtyEnvironment(integration, isZsh, isBash, isFish, cols, rows, remoteHostToken);

    int master = 0;
    // Let forkpty configure the slave's normal terminal defaults. Passing a
    // zero-initialized termios here disables canonical input and echo.
    const int descriptorLimit = inheritedDescriptorLimit();
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        execShellInChild(shellBytes, argv, envp, initialDirectory, homePath, descriptorLimit);
    }

    // Parent. Shells started later close inherited descriptors themselves;
    // close-on-exec also keeps the master out of anything else Konsoll runs.
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    m_masterFd = master;
    m_pid = pid;
    m_session = pid;

    // Non-blocking reads so the notifier never blocks the GUI thread.
    const int fl = fcntl(m_masterFd, F_GETFL, 0);
    const unsigned int newFl = static_cast<unsigned int>(fl) | static_cast<unsigned int>(O_NONBLOCK);
    fcntl(m_masterFd, F_SETFL, static_cast<int>(newFl));

    m_notifier = std::make_unique<QSocketNotifier>(m_masterFd, QSocketNotifier::Read, this);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &PtyProcess::onReadable);
    m_writeNotifier = std::make_unique<QSocketNotifier>(m_masterFd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier.get(), &QSocketNotifier::activated, this, &PtyProcess::onWritable);
    return true;
}

void PtyProcess::write(const QByteArray &data)
{
    if (m_masterFd < 0 || data.isEmpty()) {
        return;
    }
    // Behind earlier input still waiting, so bytes never reorder.
    m_pendingInput.append(data);
    flushPendingInput();
}

void PtyProcess::onWritable()
{
    flushPendingInput();
}

void PtyProcess::flushPendingInput()
{
    // The PTY input queue is small (about 1 KiB on macOS): a paste larger
    // than that fills it until the program reads. Dropping the rest would
    // also drop a bracketed paste's closing ESC[201~ and leave the program
    // stuck in paste mode, so keep it and resume when the queue drains.
    bool wouldBlock = false;
    while (m_masterFd >= 0 && !wouldBlock && m_pendingOffset < m_pendingInput.size()) {
        const ssize_t n = ::write(
            m_masterFd,
            m_pendingInput.constData() + m_pendingOffset,
            static_cast<std::size_t>(m_pendingInput.size() - m_pendingOffset)
        );
        if (n > 0) {
            m_pendingOffset += static_cast<int>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wouldBlock = true;
        } else if (n == 0 || errno != EINTR) {
            // The slave side is gone; the read side reports EOF.
            m_pendingInput.clear();
            m_pendingOffset = 0;
        }
    }
    if (m_pendingOffset >= m_pendingInput.size()) {
        m_pendingInput.clear();
        m_pendingOffset = 0;
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(!m_pendingInput.isEmpty());
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
    signalSession(SIGHUP);
}

void PtyProcess::kill() const
{
    signalSession(SIGKILL);
}

bool PtyProcess::sessionHasProcesses() const
{
    return m_pid > 0 || (m_session > 0 && !sessionProcessGroups(static_cast<pid_t>(m_session)).empty());
}

void PtyProcess::signalSession(int signal) const
{
    if (m_session <= 0) {
        return;
    }
    // The shell's group alone misses its jobs: job control runs each in a
    // group of its own (ssh, codex, vim), reached otherwise only by the
    // terminal hangup and by the shell forwarding SIGHUP, and a job that
    // ignores SIGHUP or outlives a killed shell would keep running.
    const auto session = static_cast<pid_t>(m_session);
    std::vector<pid_t> groups = sessionProcessGroups(session);
    if (m_pid > 0 && std::ranges::find(groups, session) == groups.end()) {
        groups.push_back(session);
    }
    std::ranges::for_each(groups, [signal](pid_t group) { ::kill(-group, signal); });
    if (m_pid > 0) {
        // The shell itself, should it have changed its group.
        ::kill(static_cast<pid_t>(m_pid), signal);
    }
}

std::optional<int> PtyProcess::tryReap()
{
    if (m_pid <= 0) {
        return -1;
    }
    int status = 0;
    const pid_t result = reapWithoutBlocking(static_cast<pid_t>(m_pid), status);
    if (result == 0) {
        return std::nullopt;
    }
    m_pid = -1;
    // result < 0: no longer our child (ECHILD); nothing left to wait for.
    return result > 0 ? exitCodeOf(status) : -1;
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
        return programName(
            executable,
            isScriptInterpreter(QFileInfo(executable).fileName()) ? processArguments(group) : QStringList{}
        );
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
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(false);
    }
    m_pendingInput.clear();
    m_pendingOffset = 0;
    if (m_masterFd >= 0) {
        ::close(m_masterFd);
        m_masterFd = -1;
    }
}

} // namespace konsoll
