#include "TerminalBackend.h"

#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QProcess>
#include <QUrl>
#include <QtGui>

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <utility>

using namespace std::chrono_literals;

namespace konsoll
{

namespace
{
// Standard xterm 256-color palette (0x00RRGGBB).
std::array<QRgb, 256> buildPalette()
{
    std::array<QRgb, 256> p{};
    p[0] = qRgb(0, 0, 0);
    p[1] = qRgb(194, 54, 33);
    p[2] = qRgb(151, 185, 15);
    p[3] = qRgb(181, 138, 4);
    // Keep ANSI blue vivid enough to remain legible over the translucent
    // window when shells use the normal (non-bold) blue for their prompt.
    p[4] = qRgb(84, 110, 255);
    p[5] = qRgb(140, 83, 194);
    p[6] = qRgb(48, 190, 225);
    p[7] = qRgb(203, 204, 205);
    p[8] = qRgb(42, 42, 42);
    p[9] = qRgb(234, 92, 53);
    p[10] = qRgb(181, 192, 55);
    p[11] = qRgb(253, 177, 44);
    p[12] = qRgb(72, 118, 255);
    p[13] = qRgb(214, 108, 255);
    p[14] = qRgb(52, 189, 244);
    p[15] = qRgb(242, 242, 242);
    int i = 0;

    for (int r = 0; r < 6; ++r) {
        for (int g = 0; g < 6; ++g) {
            for (int b = 0; b < 6; ++b) {
                p[16 + i] =
                    qRgb((r != 0) ? (r * 40) + 55 : 0, (g != 0) ? (g * 40) + 55 : 0, (b != 0) ? (b * 40) + 55 : 0);
                ++i;
            }
        }
    }

    for (int i2 = 0; i2 < 24; ++i2) {
        const int v = (i2 * 10) + 8;
        p[232 + i2] = qRgb(v, v, v);
    }

    return p;
}

double linearRgbChannel(int channel)
{
    const double value = static_cast<double>(channel) / 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double relativeLuminance(QRgb color)
{
    return (0.2126 * linearRgbChannel(qRed(color))) + (0.7152 * linearRgbChannel(qGreen(color))) +
           (0.0722 * linearRgbChannel(qBlue(color)));
}

double contrastRatio(QRgb foreground, QRgb background)
{
    const double foregroundLuminance = relativeLuminance(foreground);
    const double backgroundLuminance = relativeLuminance(background);
    return (std::max(foregroundLuminance, backgroundLuminance) + 0.05) /
           (std::min(foregroundLuminance, backgroundLuminance) + 0.05);
}

QRgb readableForeground(QRgb foreground, QRgb background)
{
    const double backgroundLuminance = relativeLuminance(background);
    if (backgroundLuminance <= 0.5 || contrastRatio(foreground, background) >= 4.5) {
        return foreground;
    }

    const double foregroundLuminance = relativeLuminance(foreground);
    if (foregroundLuminance <= 0.0) {
        return foreground;
    }

    const double targetLuminance = ((backgroundLuminance + 0.05) / 4.5) - 0.05;
    const double scale = std::clamp(targetLuminance / foregroundLuminance, 0.0, 1.0);
    const auto toSrgb = [scale](int channel) {
        const double linear = linearRgbChannel(channel) * scale;
        const double value = linear <= 0.0031308 ? linear * 12.92 : (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
        return static_cast<int>(std::lround(value * 255.0));
    };
    return qRgb(toSrgb(qRed(foreground)), toSrgb(qGreen(foreground)), toSrgb(qBlue(foreground)));
}

// Encode a Ctrl+<key> press into the control byte the shell expects, or an
// empty byte array if the key has no Ctrl meaning.
QByteArray ctrlKeyByte(const QString &text, bool shift)
{
    if (text.isEmpty()) {
        return {};
    }
    const QChar c = text.at(0);
    if (c.unicode() < 0x20) { // 0x20: below are ASCII control chars; pass through raw
        return QByteArray(1, static_cast<char>(c.unicode()));
    }
    if (shift) {
        const QChar u = c.toUpper();
        if (u >= 'A' && u <= 'Z') {
            return QByteArray(1, static_cast<char>((u.unicode() - 'A' + 1) & 0x1fu));
        }
    } else {
        const QChar l = c.toLower();
        if (l >= 'a' && l <= 'z') {
            return QByteArray(1, static_cast<char>((l.unicode() - 'a' + 1) & 0x1fu));
        }
    }
    return {};
}

// Map a plain (unmodified) special key to its byte sequence, or an empty
// byte array if the key has no special meaning.
QByteArray specialKeyByte(int key, bool applicationCursorKeys)
{
    switch (key) {
    case static_cast<int>(Qt::Key_Return):
    case static_cast<int>(Qt::Key_Enter):
        return QByteArray(1, static_cast<char>(0x0d));
    case static_cast<int>(Qt::Key_Backspace):
        return QByteArray(1, static_cast<char>(0x7f));
    case static_cast<int>(Qt::Key_Tab):
        return QByteArray(1, static_cast<char>(0x09));
    case static_cast<int>(Qt::Key_Escape):
        return QByteArray(1, static_cast<char>(0x1b));
    case static_cast<int>(Qt::Key_Up):
        return applicationCursorKeys ? QByteArray("\x1bOA") : QByteArray("\x1b[A");
    case static_cast<int>(Qt::Key_Down):
        return applicationCursorKeys ? QByteArray("\x1bOB") : QByteArray("\x1b[B");
    case static_cast<int>(Qt::Key_Right):
        return applicationCursorKeys ? QByteArray("\x1bOC") : QByteArray("\x1b[C");
    case static_cast<int>(Qt::Key_Left):
        return applicationCursorKeys ? QByteArray("\x1bOD") : QByteArray("\x1b[D");
    case static_cast<int>(Qt::Key_Home):
        return "\x1bOH";
    case static_cast<int>(Qt::Key_End):
        return "\x1bOF";
    case static_cast<int>(Qt::Key_Delete):
        return "\x1b[3~";
    case static_cast<int>(Qt::Key_PageUp):
        return "\x1b[5~";
    case static_cast<int>(Qt::Key_PageDown):
        return "\x1b[6~";
    default:
        return {};
    }
}

// Map an Option-modified key to its byte sequence (word movement and
// deletion for the shell line editor), or an empty byte array if the key
// has no Option meaning.
QByteArray optionKeyByte(int key)
{
    switch (key) {
    case static_cast<int>(Qt::Key_Period):
        // Readline and zle use Meta-. for inserting the previous command's
        // last argument (and cycling through older history entries).
        return "\033.";
    case static_cast<int>(Qt::Key_Left):
        return "\033b";
    case static_cast<int>(Qt::Key_Right):
        return "\033f";
    case static_cast<int>(Qt::Key_Backspace):
        return "\033\177";
    case static_cast<int>(Qt::Key_Delete):
        return "\033D";
    default:
        return {};
    }
}
} // namespace

TerminalBackend::TerminalBackend(QObject *parent) : QObject(parent)
{
    m_frameTimer.setTimerType(Qt::CoarseTimer);
    m_frameTimer.setInterval(16ms);
    m_frameTimer.setSingleShot(true);
    connect(&m_frameTimer, &QTimer::timeout, this, &TerminalBackend::onFrameTimer);

    const QPalette palette = QGuiApplication::palette();
    setSystemColors(palette.color(QPalette::Window), palette.color(QPalette::WindowText));
}

TerminalBackend::~TerminalBackend()
{
    close();
}

void TerminalBackend::setSystemColors(const QColor &background, const QColor &foreground)
{
    const unsigned int newBackground = background.rgba();
    const unsigned int newForeground = foreground.rgba();
    if (m_defaultBg == newBackground && m_defaultFg == newForeground) {
        return;
    }

    m_defaultBg = newBackground;
    m_defaultFg = newForeground;
    emit colorSchemeChanged();
    if (m_terminal) {
        scheduleFrame();
    }
}

void TerminalBackend::start(const QString &workingDirectory)
{
    if (m_state != SessionState::Created) {
        return; // Already started or closed.
    }

    m_terminal = std::make_unique<Terminal>(m_rows, m_cols);
    m_worker = std::make_unique<PtyWorker>();
    m_thread = std::make_unique<QThread>();
    m_worker->moveToThread(m_thread.get());
    connect(m_worker.get(), &PtyWorker::outputReceived, this, &TerminalBackend::onData);
    connect(m_worker.get(), &PtyWorker::started, this, &TerminalBackend::onStarted);
    connect(m_worker.get(), &PtyWorker::processExited, this, &TerminalBackend::onExited);
    connect(m_worker.get(), &PtyWorker::startFailed, this, &TerminalBackend::onStartFailed);
    connect(m_worker.get(), &PtyWorker::foregroundProcessChanged, this, &TerminalBackend::onForegroundChanged);
    connect(m_worker.get(), &PtyWorker::shutdownComplete, m_thread.get(), &QThread::quit, Qt::DirectConnection);
    connect(m_thread.get(), &QThread::finished, this, &TerminalBackend::onWorkerThreadStopped);
    m_thread->start();
    const QString shell = detectDefaultShell();
    QMetaObject::invokeMethod(m_worker.get(), &PtyWorker::start, Qt::QueuedConnection, shell, m_cols, m_rows, workingDirectory);
    m_state = SessionState::Starting;
}

bool TerminalBackend::close()
{
    if (m_state == SessionState::Stopped) {
        return true;
    }
    if (m_state != SessionState::Stopping) {
        m_state = SessionState::Stopping;
        if (m_worker) {
            QMetaObject::invokeMethod(m_worker.get(), &PtyWorker::shutdown, Qt::QueuedConnection);
        } else if (m_thread) {
            m_thread->quit();
        }
    }
    if (m_thread) {
        constexpr unsigned long kThreadJoinTimeoutMs = 3000;
        if (!m_thread->wait(kThreadJoinTimeoutMs)) {
            qWarning("Konsoll: session worker did not stop within %lu ms; deferring session removal",
                     kThreadJoinTimeoutMs);
            return false;
        }
    }
    m_state = SessionState::Stopped;
    return true;
}

void TerminalBackend::onData(const QByteArray &data)
{
    const bool wasMouseTracking = mouseTracking();
    const bool wasMouseMotionTracking = mouseMotionTracking();
    const bool wasCommandRunning = commandRunning();
    m_terminal->write(data);
    if (m_terminal->clipboardTextDirty()) {
        QGuiApplication::clipboard()->setText(m_terminal->clipboardText());
        m_terminal->clearClipboardTextDirty();
    }
    if (m_terminal->commandFinished()) {
        emit commandFinished();
        m_terminal->clearCommandFinished();
    }
    if (wasMouseTracking != mouseTracking() || wasMouseMotionTracking != mouseMotionTracking()) {
        emit mouseTrackingChanged();
    }
    if (wasCommandRunning != commandRunning()) {
        emit commandRunningChanged();
    }
    if (m_terminal->keyboardProtocolQueryPending()) {
        writeToPty("\x1b[?1u");
        m_terminal->clearKeyboardProtocolQueryPending();
    }
    if (m_terminal->daPending()) {
        writeToPty(Terminal::daResponse());
        m_terminal->clearDaPending();
    }
    if (m_terminal->colorQuery() != 0) {
        writeToPty(Terminal::colorQueryResponse(m_terminal->colorQuery()));
        m_terminal->clearColorQuery();
    }
    // Neovim pairs OSC 11 with CSI 5n and expects the OSC color reply first;
    // the following DSR response confirms that the color query was supported.
    if (m_terminal->dsrQuery() != 0) {
        writeToPty(m_terminal->dsrResponse());
        m_terminal->clearDsrQuery();
    }

    if (m_terminal->titleDirty()) {
        m_title = m_terminal->title();
        emit titleChanged();
        m_terminal->clearTitleDirty();
    }
    if (m_terminal->currentDirectoryDirty()) {
        m_currentDirectory = m_terminal->currentDirectory();
        emit currentDirectoryChanged();
        m_terminal->clearCurrentDirectoryDirty();
    }

    if (m_terminal->clearPending()) {
        // The screen was erased (e.g. Ctrl+L / clear); a selection over the
        // now-blank lines is stale, so drop it.
        m_selAnchor = {.line = -1, .col = 0};
        m_selCurrent = {.line = -1, .col = 0};
        emit selectionChanged();
        // The highlighted line is in the erased screen, so clear it.
        clearHighlight();
        m_terminal->clearCommandBlocks();
        m_terminal->clearClearPending();
    }

    checkScrollOffsetChanged();
    scheduleFrame();
    // Acknowledge consumption to the worker so it can resume paused reads.
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker.get(), &PtyWorker::outputDelivered, Qt::QueuedConnection,
                                  static_cast<qint64>(data.size()));
    }
}

void TerminalBackend::onStarted()
{
    m_state = SessionState::Running;
}

void TerminalBackend::onExited(int code)
{
    emit processExited(code);
}

void TerminalBackend::onStartFailed(const QString &error)
{
    m_state = SessionState::Failed;
    qWarning("Konsoll: failed to start the PTY: %s", qUtf8Printable(error));
    emit startFailed(error);
}

void TerminalBackend::onWorkerThreadStopped()
{
    m_state = SessionState::Stopped;
    emit workerThreadStopped();
}

void TerminalBackend::onForegroundChanged(bool hasForeground)
{
    // The worker reports on every poll; cache only on change.
    if (hasForeground == m_hasForeground) {
        return;
    }
    m_hasForeground = hasForeground;
}

void TerminalBackend::writeToPty(const QByteArray &data)
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker.get(), &PtyWorker::write, Qt::QueuedConnection, data);
    }
}

void TerminalBackend::resizePty(int columns, int rows)
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker.get(), &PtyWorker::resize, Qt::QueuedConnection, columns, rows);
    }
}

void TerminalBackend::onFrameTimer()
{
    emit frameReady();
}

void TerminalBackend::scheduleFrame()
{
    if (!m_frameTimer.isActive()) {
        m_frameTimer.start();
    }
}

void TerminalBackend::keyPress(int key, const QString &text, int modifiers)
{
    const unsigned int mods = static_cast<unsigned int>(modifiers);
    // In this QML key-event path on macOS, ControlModifier is used for app
    // shortcuts (Command), while MetaModifier carries the physical Control
    // key. Keep shell control bytes distinct from app shortcuts.
    const bool command = (mods & static_cast<unsigned int>(Qt::ControlModifier)) != 0;
    const bool control = (mods & static_cast<unsigned int>(Qt::MetaModifier)) != 0;
    const bool option = (mods & static_cast<unsigned int>(Qt::AltModifier)) != 0;
    const bool shift = (mods & static_cast<unsigned int>(Qt::ShiftModifier)) != 0;

    // Editor shortcuts: copy / paste / select-all.
    if (command) {
        editorShortcuts(key);
        return;
    }

    // Typing or navigating on from the current text action clears its visual
    // state, including both a command highlight and a text selection.
    clearTextSelectionAndHighlight();

    // Typing while scrolled up snaps the view back to the live cursor.
    const bool pageNavigation = key == static_cast<int>(Qt::Key_PageUp) || key == static_cast<int>(Qt::Key_PageDown);
    if (!pageNavigation && m_terminal && m_terminal->screen().scrollOffset() > 0) {
        scrollToBottom();
    }

    QByteArray out;
    if (shift && !control && (key == static_cast<int>(Qt::Key_Return) || key == static_cast<int>(Qt::Key_Enter))) {
        // Codex and other terminal TUIs commonly bind LF (Ctrl+J) to insert
        // a draft newline. TUIs using Kitty keyboard mode need the distinct
        // modified Enter event instead.
        out = m_terminal && m_terminal->kittyKeyboardEnabled() ? QByteArray("\x1b[13;2u") : QByteArray("\n");
    } else if (control) {
        // Physical Ctrl+K: clear the scrollback buffer (terminal-level action,
        // like Konsole's "Clear Buffer"). Not sent to the shell.
        if (key == static_cast<int>(Qt::Key_K)) {
            clearBuffer();
            return;
        }
        // Physical Ctrl+letter: send the control byte (e.g. Ctrl+C -> 0x03).
        // The event text may be empty for a Ctrl-modified key, so derive the
        // letter from the key code if needed.
        QString ctrlText = text;
        if (ctrlText.isEmpty() && key >= static_cast<int>(Qt::Key_A) && key <= static_cast<int>(Qt::Key_Z)) {
            ctrlText = QChar(static_cast<char>('a' + (key - static_cast<int>(Qt::Key_A))));
        }
        out = ctrlKeyByte(ctrlText, shift);
    } else {
        out = specialKeyByte(key, m_terminal && m_terminal->applicationCursorKeys());
        if (option) {
            // Option-modified keys: word-by-word movement and deletion,
            // handled by the shell line editor (zsh/bash: \eb/\ef ->
            // word-left/right, \e\177 -> backward-kill-word, \eD ->
            // forward-kill-word).
            const QByteArray opt = optionKeyByte(key);
            if (!opt.isEmpty()) {
                out = opt;
            }
        }
        if (out.isEmpty() && !text.isEmpty()) {
            out = text.toUtf8();
        }
    }

    if (!out.isEmpty() && m_worker) {
        writeToPty(out);
    }
}

void TerminalBackend::writeText(const QString &text)
{
    if (m_worker && !text.isEmpty()) {
        writeToPty(text.toUtf8());
    }
}

void TerminalBackend::mousePress(int button, int row, int col, int modifiers)
{
    sendMouseReport(button, row, col, 0, modifiers);
}

void TerminalBackend::mouseRelease(int button, int row, int col, int modifiers)
{
    sendMouseReport(button, row, col, 1, modifiers);
}

void TerminalBackend::mouseMove(int button, int row, int col, int modifiers)
{
    sendMouseReport(button, row, col, 2, modifiers);
}

void TerminalBackend::mouseWheel(bool up, int row, int col, int modifiers)
{
    sendMouseReport(up ? 64 : 65, row, col, up ? 3 : 4, modifiers);
}

QVariantMap TerminalBackend::linkAt(int row, int col) const
{
    if (!m_terminal || row < 0 || row >= m_rows || col < 0 || col >= m_cols) {
        return {};
    }

    const Screen &screen = m_terminal->screen();
    const Line &cells = screen.lineAt(visibleStart() + row);
    QString line;
    std::ranges::for_each(std::views::iota(0, static_cast<int>(cells.size())), [&cells, &line](int index) {
        const char32_t codePoint = static_cast<char32_t>(cells.at(static_cast<std::size_t>(index)).ch);
        line.append(QString::fromUcs4(&codePoint, 1));
    });

    static const QRegularExpression linkPattern(
        QStringLiteral(R"((?:https?|ftp|file)://[^\s<>"']+|mailto:[^\s<>"']+|www\.[^\s<>"']+|(?:~?/|\.\.?/)[^\s<>"']+)")
    );
    auto matches = linkPattern.globalMatch(line);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (col < match.capturedStart() || col >= match.capturedEnd()) {
            continue;
        }

        QString target = match.captured();
        while (!target.isEmpty() && QStringLiteral(".,;:!?) ]}").contains(target.back())) {
            target.chop(1);
        }
        if (target.isEmpty()) {
            return {};
        }
        const int targetLength = target.size();
        if (col >= match.capturedStart() + targetLength) {
            return {};
        }

        if (target.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) {
            target.prepend(QStringLiteral("https://"));
        }
        const QUrl url(target);
        if (url.isValid() && !url.scheme().isEmpty()) {
            if (url.isLocalFile() && !QFileInfo::exists(url.toLocalFile())) {
                return {};
            }
            return {
                {QStringLiteral("target"), url.toString()},
                {QStringLiteral("start"), match.capturedStart()},
                {QStringLiteral("end"), match.capturedStart() + targetLength}
            };
        }

        if (target.startsWith(QLatin1Char('~'))) {
            target.replace(0, 1, QDir::homePath());
        } else if (!QDir::isAbsolutePath(target)) {
            target = QDir(m_currentDirectory).filePath(target);
        }
        const QFileInfo fileInfo(QDir::cleanPath(target));
        if (!fileInfo.exists()) {
            return {};
        }
        return {
            {QStringLiteral("target"), QUrl::fromLocalFile(fileInfo.absoluteFilePath()).toString()},
            {QStringLiteral("start"), match.capturedStart()},
            {QStringLiteral("end"), match.capturedStart() + targetLength}
        };
    }
    return {};
}

bool TerminalBackend::openLinkAt(int row, int col) const
{
    const QVariantMap link = linkAt(row, col);
    if (link.isEmpty()) {
        return false;
    }
    const QUrl url(link.value(QStringLiteral("target")).toString());
    if (url.isLocalFile()) {
        const QFileInfo fileInfo(url.toLocalFile());
        if (fileInfo.isDir()) {
            return QProcess::startDetached(
                QStringLiteral("/usr/bin/open"),
                {QStringLiteral("-a"), QStringLiteral("Finder"), fileInfo.absoluteFilePath()}
            );
        }
    }
    return QDesktopServices::openUrl(url);
}

void TerminalBackend::sendMouseReport(int button, int row, int col, int event, int modifiers)
{
    if (!m_worker || !m_terminal) {
        return;
    }
    unsigned int mouseModifiers = 0;
    if ((modifiers & Qt::ShiftModifier) != 0) {
        mouseModifiers |= 1u;
    }
    if ((modifiers & Qt::AltModifier) != 0) {
        mouseModifiers |= 2u;
    }
    if ((modifiers & Qt::ControlModifier) != 0) {
        mouseModifiers |= 4u;
    }
    writeToPty(m_terminal->mouseReport(button, row, col, event, mouseModifiers));
}

void TerminalBackend::resize(int cols, int rows)
{
    if (cols < 2 || rows < 2 || (cols == m_cols && rows == m_rows)) {
        return;
    }

    m_cols = cols;
    m_rows = rows;

    if (m_terminal) {
        // Terminal/Screen take (rows, cols); the PTY takes (cols, rows).
        m_terminal->resize(rows, cols);
    }

    if (m_worker) {
        resizePty(cols, rows);
    }

    // The reflow re-wraps the history, so absolute selection anchors are stale.
    m_selAnchor = {.line = -1, .col = 0};
    m_selCurrent = {.line = -1, .col = 0};

    // The reflow re-wraps the history, so the highlighted line is stale.
    clearHighlight();

    emit selectionChanged();
    emit sizeChanged();

    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::scrollBy(int delta)
{
    if (!m_terminal) {
        return;
    }

    m_terminal->screen().scrollBy(delta);
    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::scrollTo(int offset)
{
    if (!m_terminal) {
        return;
    }

    m_terminal->screen().scrollTo(offset);
    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::scrollToBottom()
{
    if (!m_terminal) {
        return;
    }

    m_terminal->screen().scrollToBottom();
    checkScrollOffsetChanged();
    scheduleFrame();
}

void TerminalBackend::clearScreen()
{
    keyPress(static_cast<int>(Qt::Key_L), {}, static_cast<int>(Qt::MetaModifier));
}

void TerminalBackend::clearBuffer()
{
    if (!m_terminal) {
        return;
    }

    // Drop the history, then let the shell handle Ctrl+L so its line editor
    // clears the display and redraws the live prompt at the correct position.
    // Clearing the screen model and moving its cursor directly would leave it
    // out of sync with the shell's terminal state; the next Enter would then
    // reveal/redraw the prompt from that stale state.
    Screen &s = m_terminal->screen();
    s.clearScrollback();
    m_terminal->clearCommandBlocks();

    if (m_worker) {
        writeToPty(QByteArray(1, '\x0c'));
    } else {
        s.eraseInDisplay(2);
        s.moveCursor(0, 0);
    }

    // A selection over now-gone lines is stale, so drop it.
    m_selAnchor = {.line = -1, .col = 0};
    m_selCurrent = {.line = -1, .col = 0};
    emit selectionChanged();

    // The highlighted line is in the dropped history, so clear it.
    clearHighlight();

    checkScrollOffsetChanged();
    scheduleFrame();
}

int TerminalBackend::search(const QString &query, bool regex)
{
    m_searchMatches.clear();
    m_searchIndex = -1;
    if (!m_terminal || query.isEmpty()) {
        scheduleFrame();
        return 0;
    }
    Screen &screen = m_terminal->screen();
    const QRegularExpression expression(regex ? query : QRegularExpression::escape(query));
    if (!expression.isValid()) {
        scheduleFrame();
        return -1;
    }
    for (int lineIndex = 0; lineIndex < screen.totalLines(); ++lineIndex) {
        const Line &line = screen.lineAt(lineIndex);
        QString text;
        QVector<int> offsetToCell;
        text.reserve(static_cast<qsizetype>(line.size()));
        for (int cellIndex = 0; cellIndex < static_cast<int>(line.size()); ++cellIndex) {
            const Cell &cell = line.at(static_cast<std::size_t>(cellIndex));
            const char32_t cp = static_cast<char32_t>(cell.ch);
            const QString glyph = QString::fromUcs4(&cp, 1);
            for (qsizetype unit = 0; unit < glyph.size(); ++unit) {
                offsetToCell.append(cellIndex);
            }
            text.append(glyph);
        }
        offsetToCell.append(static_cast<int>(line.size()));
        auto it = expression.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            if (match.capturedLength() > 0) {
                const int start = offsetToCell.at(match.capturedStart());
                const int end = offsetToCell.at(match.capturedStart() + match.capturedLength());
                if (end > start) {
                    m_searchMatches.append({.line = lineIndex, .start = start, .end = end});
                }
            }
        }
    }
    if (!m_searchMatches.isEmpty()) {
        m_searchIndex = 0;
        const int start = m_searchMatches.first().line;
        screen.scrollTo(screen.totalLines() - screen.rows() - start);
        checkScrollOffsetChanged();
    }
    scheduleFrame();
    return m_searchMatches.size();
}

int TerminalBackend::searchNext(bool backwards)
{
    if (m_searchMatches.isEmpty()) {
        return 0;
    }
    const int count = m_searchMatches.size();
    m_searchIndex = (m_searchIndex + (backwards ? count - 1 : 1)) % count;
    const auto &match = m_searchMatches.at(m_searchIndex);
    m_terminal->screen().scrollTo(m_terminal->screen().totalLines() - m_terminal->screen().rows() - match.line);
    checkScrollOffsetChanged();
    scheduleFrame();
    return count;
}

int TerminalBackend::searchCurrentIndex() const
{
    return m_searchIndex >= 0 && m_searchIndex < m_searchMatches.size() ? m_searchIndex + 1 : 0;
}

void TerminalBackend::clearSearch()
{
    m_searchMatches.clear();
    m_searchIndex = -1;
    scheduleFrame();
}

int TerminalBackend::searchRowLeft(int visibleRow) const
{
    if (m_searchIndex < 0 || m_searchIndex >= m_searchMatches.size()) {
        return -1;
    }
    const auto &match = m_searchMatches.at(m_searchIndex);
    return match.line == visibleStart() + visibleRow ? match.start : -1;
}

int TerminalBackend::searchRowRight(int visibleRow) const
{
    if (m_searchIndex < 0 || m_searchIndex >= m_searchMatches.size()) {
        return -1;
    }
    const auto &match = m_searchMatches.at(m_searchIndex);
    return match.line == visibleStart() + visibleRow ? match.end - 1 : -1;
}

void TerminalBackend::checkScrollOffsetChanged()
{
    if (m_terminal) {
        emit scrollOffsetChanged();
    }
}

int TerminalBackend::visibleStart() const
{
    if (!m_terminal) {
        return 0;
    }

    const Screen &s = m_terminal->screen();
    return s.totalLines() - s.rows() - s.scrollOffset();
}

void TerminalBackend::selPress(int row, int col)
{
    if (!m_terminal) {
        return;
    }

    const int absLine = visibleStart() + row;
    const int c = std::clamp(col, 0, m_terminal->screen().cols() - 1);
    m_selAnchor = {.line = absLine, .col = c};
    m_selCurrent = {.line = absLine, .col = c};

    emit selectionChanged();
}

void TerminalBackend::selMove(int row, int col)
{
    if (!m_terminal || m_selAnchor.line < 0) {
        return;
    }

    Screen &s = m_terminal->screen();
    int absLine = visibleStart() + row;
    const int c = std::clamp(col, 0, s.cols() - 1);
    // Rubber-band auto-scroll: dragging into the top/bottom edge reveals more
    // history, like selecting text in any editor. The selection follows the
    // mouse in absolute coordinates, so it stays on the same text.
    bool scrolled = false;
    if (row <= 0 && s.scrollOffset() < s.maxScrollOffset()) {
        s.scrollBy(1); // up = older lines
        absLine = visibleStart();
        scrolled = true;
    } else if (row >= s.rows() - 1 && s.scrollOffset() > 0) {
        s.scrollBy(-1); // down = newer lines
        absLine = visibleStart() + (s.rows() - 1);
        scrolled = true;
    }

    m_selCurrent = {.line = absLine, .col = c};
    emit selectionChanged();

    if (scrolled) {
        checkScrollOffsetChanged();
        scheduleFrame();
    }
}

void TerminalBackend::selRelease()
{
    if (m_selAnchor.line < 0) {
        return;
    }

    // A click that didn't drag collapses to a point: clear the selection.
    if (m_selAnchor.line == m_selCurrent.line && m_selAnchor.col == m_selCurrent.col) {
        m_selAnchor = {.line = -1, .col = 0};
        m_selCurrent = {.line = -1, .col = 0};
    }

    emit selectionChanged();
}

void TerminalBackend::selClear()
{
    if (m_selAnchor.line < 0) {
        return;
    }

    m_selAnchor = {.line = -1, .col = 0};
    m_selCurrent = {.line = -1, .col = 0};
    emit selectionChanged();
}

void TerminalBackend::selWord(int row, int col)
{
    if (!m_terminal) {
        return;
    }

    Screen const &s = m_terminal->screen();
    const int absLine = visibleStart() + row;
    const Line &line = s.lineAt(absLine);
    if (col < 0 || col >= static_cast<int>(line.size())) {
        m_selAnchor = {.line = -1, .col = 0};
        m_selCurrent = {.line = -1, .col = 0};
        emit selectionChanged();
        return;
    }

    auto const isWordChar = [](unsigned int cp) {
        if (cp >= 0x10000) {
            return false; // supplementary-plane (e.g. Nerd Font icons): not a word char
        }
        const QChar c = QChar(static_cast<ushort>(cp));
        // Treat common path and filename punctuation as part of a word, as
        // terminal emulators do when double-clicking paths (e.g. docs/foo.md).
        return c.isLetterOrNumber() || cp == '_' || cp == '-' || cp == '.' || cp == '/';
    };
    int startCol = col;
    int endCol = col;
    while (startCol > 0 && isWordChar(line[startCol - 1].ch)) {
        --startCol;
    }

    while (endCol + 1 < static_cast<int>(line.size()) && isWordChar(line[endCol + 1].ch)) {
        ++endCol;
    }

    m_selAnchor = {.line = absLine, .col = startCol};
    m_selCurrent = {.line = absLine, .col = endCol};
    emit selectionChanged();
}

void TerminalBackend::selLine(int row)
{
    if (!m_terminal) {
        return;
    }

    // The whole logical line: column 0 to its last non-space column (full
    // width for a blank line), like a text editor's triple-click.
    const Screen &s = m_terminal->screen();
    int start = visibleStart() + row;
    int end = start;
    while (s.isWrappedContinuation(start)) {
        --start;
    }
    while (end + 1 < s.totalLines() && s.isWrappedContinuation(end + 1)) {
        ++end;
    }
    m_selAnchor = {.line = start, .col = 0};
    m_selCurrent = {.line = end, .col = lineTextEnd(end)};
    emit selectionChanged();
}

void TerminalBackend::selectAll()
{
    if (!m_terminal) {
        return;
    }

    Screen const &s = m_terminal->screen();
    const int start = visibleStart();
    m_selAnchor = {.line = start, .col = 0};
    m_selCurrent = {.line = start + s.rows() - 1, .col = s.cols() - 1};
    emit selectionChanged();
}

void TerminalBackend::highlightLine(int row)
{
    if (!m_terminal) {
        return;
    }

    m_highlightStart = visibleStart() + row;
    m_highlightEnd = m_highlightStart;
    emit highlightChanged();
}

void TerminalBackend::highlightCommandAt(int row)
{
    if (!m_terminal) {
        return;
    }

    const int absoluteLine = visibleStart() + row;
    if (m_highlightStart >= 0 && absoluteLine >= m_highlightStart && absoluteLine <= m_highlightEnd) {
        clearHighlight();
        return;
    }

    const auto &blocks = m_terminal->commandBlocks();
    const auto reverseBlocks = std::views::reverse(blocks);
    const auto block = std::ranges::find_if(reverseBlocks, [absoluteLine](const auto &candidate) {
        return absoluteLine >= candidate.startLine && absoluteLine <= candidate.endLine;
    });
    if (block == reverseBlocks.end()) {
        const int activeStart = m_terminal->activeCommandStartLine();
        if (activeStart >= 0 && absoluteLine >= activeStart) {
            m_highlightStart = activeStart;
            m_highlightEnd = absoluteLine;
            emit highlightChanged();
        } else {
            highlightLine(row);
        }
        return;
    }

    m_highlightStart = block->startLine;
    m_highlightEnd = block->endLine;
    emit highlightChanged();
}

void TerminalBackend::clearHighlight()
{
    if (m_highlightStart < 0) {
        return;
    }

    m_highlightStart = -1;
    m_highlightEnd = -1;
    emit highlightChanged();
}

void TerminalBackend::clearTextSelectionAndHighlight()
{
    selClear();
    clearHighlight();
}

int TerminalBackend::highlightedLineEnd() const
{
    if (m_highlightStart < 0 || !m_terminal) {
        return -1;
    }
    return lineTextEnd(m_highlightEnd);
}

int TerminalBackend::highlightVisibleRow() const
{
    if (m_highlightStart < 0 || !m_terminal) {
        return -1;
    }
    const int start = visibleStart();
    const int row = m_highlightStart - start;
    if (row < 0 || row >= m_rows) {
        return -1;
    }
    return row;
}

int TerminalBackend::highlightVisibleStartRow() const
{
    if (m_highlightStart < 0 || !m_terminal) {
        return -1;
    }
    const int start = visibleStart();
    const int end = start + m_rows - 1;
    if (m_highlightEnd < start || m_highlightStart > end) {
        return -1;
    }
    return std::max(0, m_highlightStart - start);
}

int TerminalBackend::highlightVisibleEndRow() const
{
    if (m_highlightEnd < 0 || !m_terminal) {
        return -1;
    }
    const int start = visibleStart();
    const int end = start + m_rows - 1;
    if (m_highlightEnd < start || m_highlightStart > end) {
        return -1;
    }
    return std::min(m_rows - 1, m_highlightEnd - start);
}

void TerminalBackend::copySelection()
{
    if (!m_terminal) {
        return;
    }

    if (m_highlightStart >= 0 || selectionActive()) {
        QGuiApplication::clipboard()->setText(selectionText());
        clearTextSelectionAndHighlight();
        return;
    }

    if (m_selAnchor.line < 0) {
        clearTextSelectionAndHighlight();
        return;
    }

    Screen const &s = m_terminal->screen();
    // Reading-order selection: the earlier point starts the selection (its
    // line runs from that column to the end of the text), the later point
    // ends it (its line runs from column 0 to that column). Middle lines are
    // whole. Symmetric in drag direction, like selecting text in an editor.
    const int sLine = m_selAnchor.line;
    const int eLine = m_selCurrent.line;
    const int sCol = m_selAnchor.col;
    const int eCol = m_selCurrent.col;
    const int top = std::min(sLine, eLine);
    const int bot = std::max(sLine, eLine);
    const int topCol = (sLine == top) ? sCol : eCol;
    const int botCol = (eLine == bot) ? eCol : sCol;
    QString text;

    for (int line = top; line <= bot; ++line) {
        const Line &l = s.lineAt(line);
        int c0 = 0;
        int c1 = 0;
        // c1 is an exclusive end for the copy loop below; the selection points
        // are inclusive columns, so each bound gets +1.
        if (top == bot) {
            c0 = std::min(sCol, eCol);
            c1 = std::max(sCol, eCol) + 1;
        } else if (line == top) {
            c0 = topCol;
            c1 = lineTextEnd(line) + 1;
        } else if (line == bot) {
            c0 = 0;
            c1 = botCol + 1;
        } else {
            c0 = 0;
            c1 = lineTextEnd(line) + 1;
        }
        c1 = std::min(c1, static_cast<int>(l.size()));
        while (c1 > c0 && l[c1 - 1].ch == ' ') {
            --c1;
        }
        for (int c = c0; c < c1; ++c) {
            const char32_t cp = static_cast<char32_t>(l[c].ch);
            text.append(QString::fromUcs4(&cp, 1));
        }
        if (line < bot && !s.isWrappedContinuation(line + 1)) {
            text.append('\n');
        }
    }
    QGuiApplication::clipboard()->setText(text);
    clearTextSelectionAndHighlight();
}

QString TerminalBackend::selectionText() const
{
    if (!m_terminal) {
        return {};
    }
    const Screen &screen = m_terminal->screen();
    QString text;
    if (m_highlightStart >= 0) {
        std::ranges::for_each(std::views::iota(m_highlightStart, m_highlightEnd + 1), [&](int line) {
            const Line &cells = screen.lineAt(line);
            const auto reversedCells = std::views::reverse(cells);
            const auto lastContent =
                std::ranges::find_if(reversedCells, [](const Cell &cell) { return cell.ch != ' '; });
            const int end = lastContent == reversedCells.end()
                                ? -1
                                : static_cast<int>(std::distance(cells.begin(), lastContent.base())) - 1;
            std::ranges::for_each(std::views::iota(0, end + 1), [&](int col) {
                const char32_t cp = static_cast<char32_t>(cells[col].ch);
                text.append(QString::fromUcs4(&cp, 1));
            });
            if (line < m_highlightEnd) {
                text.append('\n');
            }
        });
        return text;
    }
    if (m_selAnchor.line < 0) {
        return {};
    }

    const int top = std::min(m_selAnchor.line, m_selCurrent.line);
    const int bot = std::max(m_selAnchor.line, m_selCurrent.line);
    const int topCol = (m_selAnchor.line == top) ? m_selAnchor.col : m_selCurrent.col;
    const int botCol = (m_selCurrent.line == bot) ? m_selCurrent.col : m_selAnchor.col;
    std::ranges::for_each(std::views::iota(top, bot + 1), [&](int line) {
        const Line &cells = screen.lineAt(line);
        int first = 0;
        int end = 0;
        if (top == bot) {
            first = std::min(m_selAnchor.col, m_selCurrent.col);
            end = std::max(m_selAnchor.col, m_selCurrent.col) + 1;
        } else if (line == top) {
            first = topCol;
            end = lineTextEnd(line) + 1;
        } else if (line == bot) {
            end = botCol + 1;
        } else {
            end = lineTextEnd(line) + 1;
        }
        first = std::min(first, static_cast<int>(cells.size()));
        end = std::max(first, std::min(end, static_cast<int>(cells.size())));
        const auto selectedCells = std::ranges::subrange(cells.begin() + first, cells.begin() + end);
        const auto lastContent =
            std::ranges::find_last_if(selectedCells, [](const Cell &cell) { return cell.ch != ' '; });
        end = lastContent.empty() ? first
                                  : first + static_cast<int>(std::distance(selectedCells.begin(), lastContent.end()));
        std::ranges::for_each(std::views::iota(first, end), [&](int col) {
            const char32_t cp = static_cast<char32_t>(cells[col].ch);
            text.append(QString::fromUcs4(&cp, 1));
        });
        if (line < bot && !screen.isWrappedContinuation(line + 1)) {
            text.append('\n');
        }
    });
    return text;
}

QString TerminalBackend::bufferText() const
{
    if (!m_terminal) {
        return {};
    }
    const Screen &screen = m_terminal->screen();
    QString text;
    const int totalLines = screen.totalLines();
    std::ranges::for_each(std::views::iota(0, totalLines), [&](int line) {
        const Line &cells = screen.lineAt(line);
        const auto reversedCells = std::views::reverse(cells);
        const auto lastContent = std::ranges::find_if(reversedCells, [](const Cell &cell) { return cell.ch != ' '; });
        const int end =
            lastContent == reversedCells.end() ? 0 : static_cast<int>(std::distance(cells.begin(), lastContent.base()));
        std::ranges::for_each(std::views::iota(0, end), [&](int col) {
            const char32_t cp = static_cast<char32_t>(cells[col].ch);
            text.append(QString::fromUcs4(&cp, 1));
        });
        if (line + 1 < totalLines && !screen.isWrappedContinuation(line + 1)) {
            text.append('\n');
        }
    });
    return text;
}

bool TerminalBackend::saveTextToFile(const QUrl &url, bool selectionOnly) const
{
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QString content = selectionOnly ? selectionText() : bufferText();
    const QByteArray encoded = content.toUtf8();
    return file.write(encoded) == encoded.size();
}

void TerminalBackend::paste()
{
    if (!m_worker) {
        return;
    }
    const QClipboard *const clipboard = QGuiApplication::clipboard();
    if (!clipboard->image().isNull()) {
        // Image-aware terminal applications (including Codex CLI) handle Ctrl+V
        // themselves and read the native macOS clipboard. A PTY cannot carry an
        // image object, so forward the application's image-paste key instead of
        // converting the image to a temporary path.
        writeToPty(QByteArray(1, '\x16'));
    } else {
        const QString text = clipboard->text();
        if (!text.isEmpty()) {
            writeToPty(text.toUtf8());
        }
    }
    clearTextSelectionAndHighlight();
}

void TerminalBackend::pasteSelectionOrClipboard()
{
    if (!m_worker) {
        return;
    }
    if (selectionActive() || highlightActive()) {
        const QString text = selectionText();
        if (!text.isEmpty()) {
            writeToPty(text.toUtf8());
            clearTextSelectionAndHighlight();
            return;
        }
    }
    paste();
}

int TerminalBackend::lineTextEnd(int line) const
{
    const Line &l = m_terminal->screen().lineAt(line);
    int end = static_cast<int>(l.size()) - 1;
    while (end >= 0 && l[end].ch == ' ') {
        --end;
    }
    if (end < 0) {
        return m_terminal->screen().cols() - 1; // blank line: full width
    }
    return end;
}

void TerminalBackend::editorShortcuts(int key)
{
    if (key == static_cast<int>(Qt::Key_C)) {
        copySelection();
        return;
    }

    if (key == static_cast<int>(Qt::Key_V)) {
        paste();
        return;
    }

    if (key == static_cast<int>(Qt::Key_A)) {
        selectAll();
        return;
    }
}

int TerminalBackend::selRowLeft(int visibleRow) const
{
    if (!m_terminal || m_selAnchor.line < 0) {
        return -1;
    }
    const int sLine = m_selAnchor.line;
    const int eLine = m_selCurrent.line;
    const int sCol = m_selAnchor.col;
    const int eCol = m_selCurrent.col;
    const int topLine = std::min(sLine, eLine);
    const int botLine = std::max(sLine, eLine);
    const int absLine = visibleStart() + visibleRow;
    if (absLine < topLine || absLine > botLine) {
        return -1;
    }
    if (topLine == botLine) {
        return std::min(sCol, eCol);
    }
    if (absLine == topLine) {
        // The top line is clipped at the column of the point that sits on it.
        return (sLine == topLine) ? sCol : eCol;
    }
    return 0;
}

int TerminalBackend::selRowRight(int visibleRow) const
{
    if (!m_terminal || m_selAnchor.line < 0) {
        return -1;
    }
    const int sLine = m_selAnchor.line;
    const int eLine = m_selCurrent.line;
    const int sCol = m_selAnchor.col;
    const int eCol = m_selCurrent.col;
    const int topLine = std::min(sLine, eLine);
    const int botLine = std::max(sLine, eLine);
    const int absLine = visibleStart() + visibleRow;
    if (absLine < topLine || absLine > botLine) {
        return -1;
    }
    if (topLine == botLine) {
        return std::max(sCol, eCol);
    }
    if (absLine == botLine) {
        // The bottom line is clipped at the column of the point that sits on it.
        return (eLine == botLine) ? eCol : sCol;
    }
    return lineTextEnd(absLine);
}

QVariantList TerminalBackend::snapshot() const
{
    QVariantList rowsOut;
    if (!m_terminal) {
        return rowsOut;
    }
    const Screen &s = m_terminal->screen();
    // The visible window is the last `rows` lines, shifted up by the scroll
    // offset (0 = at the bottom).
    const int total = s.totalLines();
    const int start = total - s.rows() - s.scrollOffset();
    static const Cell kBlank{};
    rowsOut.reserve(s.rows() * s.cols());
    for (int r = 0; r < s.rows(); ++r) {
        const Line &line = s.lineAt(start + r);
        for (int c = 0; c < s.cols(); ++c) {
            const Cell &cell = (c < static_cast<int>(line.size())) ? line[c] : kBlank;
            unsigned int fg = resolveColor(cell.fg, m_defaultFg);
            unsigned int bg = resolveColor(cell.bg, m_defaultBg);

            if ((cell.flags & CellFlag::Reverse) != 0) {
                std::swap(fg, bg);
            }
            fg = readableForeground(static_cast<QRgb>(fg), static_cast<QRgb>(bg));

            // Encode the code point as a QString (surrogate pair when
            // supplementary) so the QML renderer draws one glyph per cell.
            const char32_t cp = static_cast<char32_t>(cell.ch);
            QVariantMap m;
            m["ch"] = QString::fromUcs4(&cp, 1);
            m["fg"] = static_cast<int>(fg);
            m["bg"] = static_cast<int>(bg);
            m["flags"] = static_cast<int>(cell.flags);
            rowsOut.append(m);
        }
    }
    return rowsOut;
}

int TerminalBackend::cursorRow() const
{
    return m_terminal ? m_terminal->screen().cursorRow() : 0;
}

int TerminalBackend::cursorCol() const
{
    return m_terminal ? m_terminal->screen().cursorCol() : 0;
}

bool TerminalBackend::cursorVisible() const
{
    return m_terminal ? m_terminal->screen().cursorVisible() : true;
}

QString TerminalBackend::detectDefaultShell()
{
    struct passwd pwd{};
    struct passwd *result = nullptr;
    std::array<char, 4096> buf;
    if (getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &result) == 0 && (result != nullptr)) {
        const QString shell = QString::fromLocal8Bit(result->pw_shell);
        if (!shell.isEmpty() && QFileInfo(shell).isExecutable()) {
            return shell;
        }
    }
    if (QFileInfo::exists("/bin/zsh")) {
        return "/bin/zsh";
    }
    return "/bin/sh";
}

QRgb TerminalBackend::resolveColor(
    unsigned int encoded,
    int defaultColor
) // NOLINT: encoded/defaultColor are inherently swappable
{
    if (encoded == 0) {
        return static_cast<QRgb>(defaultColor);
    }
    if (encoded <= 256) {
        static const std::array<QRgb, 256> palette = buildPalette();
        return static_cast<QRgb>(0xff000000u | palette[encoded - 1]);
    }
    if ((encoded & 0x80000000u) != 0u) {
        const unsigned int rgb = encoded & 0xffffff;
        return static_cast<QRgb>(0xff000000u | rgb);
    }
    return static_cast<QRgb>(defaultColor);
}

} // namespace konsoll
