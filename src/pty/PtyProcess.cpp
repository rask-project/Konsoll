#include "PtyProcess.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QLoggingCategory>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <util.h>

namespace konsoll
{

PtyProcess::PtyProcess(QObject *parent) : QObject(parent) {}

PtyProcess::~PtyProcess()
{
    closeMaster();
    if (m_pid > 0) {
        int status = 0;
        waitpid(m_pid, &status, 0);
        m_pid = -1;
    }
}

bool PtyProcess::start(const QString &shellPath, int cols, int rows)
{
    struct winsize ws;
    ws.ws_col = static_cast<short>(cols);
    ws.ws_row = static_cast<short>(rows);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    int master = 0;
    // Let forkpty configure the slave's normal terminal defaults. Passing a
    // zero-initialized termios here disables canonical input and echo.
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        // Child: exec the shell as a login shell (argv[0] = "-<name>").
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLUMNS", QByteArray::number(cols).constData(), 1);
        ::setenv("LINES", QByteArray::number(rows).constData(), 1);

        const QByteArray shellBytes = shellPath.toLocal8Bit();
        QByteArray argv0 = "-" + QFileInfo(shellPath).baseName().toLocal8Bit();
        char *const argv[] = {argv0.data(), nullptr}; // NOLINT: execvp needs char *const argv[]
        ::execvp(shellBytes.constData(), argv);
        ::_exit(127);
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
        ::kill(m_pid, SIGHUP);
    }
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
        int status = 0;
        waitpid(m_pid, &status, 0);
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        closeMaster();
        emit processExited(code);
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
