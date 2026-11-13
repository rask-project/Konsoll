#include "TerminalBackend.h"

#include <QDateTime>

#include "KeyEncoder.h"
#include "NativeDialogs.h"
#include "NativeKeyTracker.h"
#include "Notifications.h"
#include "ProcessKind.h"
#include "ScrollbackSettings.h"
#include "term/Grapheme.h"
#include "term/Palette.h"

#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QSettings>
#include <QUrl>
#include <QUuid>
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

// Quiet time after the last chunk of output before a frame is painted.
constexpr std::chrono::milliseconds kFrameDelay{16};
// A "prompt" the agent itself sends to wake up (Claude Code's
// <task-notification> when a background command ends, <system-reminder>,
// ...): not the user's task.
bool isAgentSystemMessage(const QString &text)
{
    static const QRegularExpression tag(QStringLiteral("^<[a-z][a-z_-]*>"));
    return tag.match(text).hasMatch();
}

// How much of a command's output "Send Last Command to Agent" carries.
constexpr qsizetype kSentOutputLength = 8000;

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

// Append a cell's character to copied/saved text. The trailing half of a wide
// character carries none.
void appendCellText(QString &text, const Cell &cell)
{
    if ((cell.flags & CellFlag::WideSpacer) != 0u) {
        return;
    }
    text.append(cellText(cell.ch)); // a code point or a grapheme cluster
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

} // namespace

TerminalBackend::TerminalBackend(QObject *parent) : QObject(parent)
{
    static int nextPaneId = 0;
    m_paneId = ++nextPaneId;
    m_remoteHostToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Agent hooks name the session by its token (KONSOLL_SESSION_TOKEN).
    connect(
        &AgentStatusServer::instance(),
        &AgentStatusServer::statusReceived,
        this,
        [this](const QString &token, const QString &agent, AgentState state, const QString &message) {
            if (token == m_remoteHostToken) {
                setAgentStatus(state, agent, message);
            }
        }
    );
    m_frameTimer.setTimerType(Qt::CoarseTimer);
    m_frameTimer.setInterval(kFrameDelay);
    m_frameTimer.setSingleShot(true);
    connect(&m_frameTimer, &QTimer::timeout, this, &TerminalBackend::onFrameTimer);
    m_animationTimer.setTimerType(Qt::PreciseTimer);
    m_animationTimer.setInterval(16ms);
    connect(&m_animationTimer, &QTimer::timeout, this, &TerminalBackend::onAnimationTimer);
    // Selection/highlight changes repaint through the same coalesced,
    // DEC 2026-aware frame as PTY output; frameReady is the only repaint
    // signal the renderer listens to.
    connect(this, &TerminalBackend::selectionChanged, this, &TerminalBackend::scheduleFrame);
    connect(this, &TerminalBackend::highlightChanged, this, &TerminalBackend::scheduleFrame);
    // Settings > Terminal: a lower limit trims the history right away.
    connect(&ScrollbackSettings::shared(), &ScrollbackSettings::maxLinesChanged, this, [this] {
        if (m_terminal) {
            m_terminal->screen().setMaxScrollback(ScrollbackSettings::shared().maxLines());
            emit scrollOffsetChanged();
            scheduleFrame();
        }
    });
    connect(&m_transferProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        const TransferJob completed = std::exchange(m_activeTransfer, TransferJob{});
        if (completed.removeLocal()) {
            QFile::remove(completed.localPath);
        }
        const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
        if (succeeded && m_state == SessionState::Running && completed.targetHost == m_remoteHost) {
            QByteArray pastedPath = completed.remotePath.toUtf8();
            if (m_terminal && m_terminal->bracketedPasteEnabled()) {
                pastedPath.prepend("\x1b[200~");
                pastedPath.append("\x1b[201~");
            }
            writeToPty(pastedPath);
        } else if (!succeeded) {
            qWarning().noquote() << "Konsoll: remote file transfer failed:" << m_transferProcess.errorString();
            emit transferFailed(tr("Remote upload failed. Check SSH access and key-based authentication."));
        }
        startNextTransfer();
    });
    connect(&m_transferProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        const TransferJob failed = std::exchange(m_activeTransfer, TransferJob{});
        if (failed.removeLocal()) {
            QFile::remove(failed.localPath);
        }
        qWarning().noquote() << "Konsoll: could not start scp:" << m_transferProcess.errorString();
        emit transferFailed(tr("Could not start scp for the remote upload."));
        startNextTransfer();
    });

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
        m_terminal->setDefaultColors(m_defaultFg, m_defaultBg);
        flushResponses(); // mode 2031 color-scheme notification
        scheduleFrame();
    }
}

void TerminalBackend::setCellPixelSize(int width, int height)
{
    m_cellPixelWidth = width;
    m_cellPixelHeight = height;
    if (m_terminal) {
        m_terminal->setCellPixelSize(width, height);
    }
}

void TerminalBackend::handleRequests()
{
    if (!m_terminal) {
        return;
    }
    const std::vector<TerminalRequest> requests = m_terminal->takeRequests();
    if (requests.empty()) {
        return; // the common case: runs after every chunk of output
    }
    const QSettings settings;
    std::ranges::for_each(requests, [this, &settings](const TerminalRequest &request) {
        switch (request.kind) {
        case TerminalRequest::ClipboardRead:
            answerClipboardRead(request.data);
            break;
        case TerminalRequest::TitleReport:
            if (settings.value(QStringLiteral("General/allowTitleReport"), false).toBool() && m_worker) {
                // Control characters would let a title inject input.
                QString title = m_title;
                title.removeIf([](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
                writeToPty((request.a == 1 ? QByteArray("\x1b]L") : QByteArray("\x1b]l")) + title.toUtf8() + "\x1b\\");
            }
            break;
        case TerminalRequest::WindowOp:
            if (settings.value(QStringLiteral("General/allowWindowOps"), false).toBool()) {
                emit windowOperationRequested(request.a, request.b, request.c);
            }
            break;
        case TerminalRequest::ColumnMode:
            // DECCOLM already resized the screen: grow / shrink the window by
            // the cell delta (QML still sees the old column count), then sync.
            emit windowOperationRequested(8, 0, request.a);
            resize(request.a, m_rows);
            break;
        case TerminalRequest::AgentReport:
            applyRemoteAgentReport(request.data);
            break;
        case TerminalRequest::Notification:
            // An agent reporting through its hooks gets Konsoll's own
            // notification (TerminalManager): its OSC 9 would repeat it.
            if (settings.value(QStringLiteral("General/allowNotifications"), true).toBool() &&
                !(m_agentState != AgentState::None &&
                  settings.value(QStringLiteral("General/agentNotifications"), true).toBool())) {
                const QString title = QString::fromUtf8(request.data);
                postNotification(title.isEmpty() ? m_title : title, QString::fromUtf8(request.body), m_remoteHostToken);
            }
            break;
        default:
            break;
        }
    });
}

// OSC 52 query: 0 never, 1 ask (default), 2 always.
void TerminalBackend::answerClipboardRead(const QByteArray &selection)
{
    const int policy = QSettings().value(QStringLiteral("General/clipboardRead"), 1).toInt();
    if (policy == 0 || !m_worker || (policy == 1 && !NativeDialogs::confirmClipboardRead(m_title))) {
        return;
    }
    const QByteArray text = QGuiApplication::clipboard()->text().toUtf8();
    writeToPty("\x1b]52;" + selection + ";" + text.toBase64() + "\x1b\\");
}

void TerminalBackend::flushResponses()
{
    if (!m_terminal) {
        return;
    }
    const QByteArray responses = m_terminal->takeResponses();
    if (!responses.isEmpty() && m_worker) {
        writeToPty(responses);
    }
}

void TerminalBackend::start(const QString &workingDirectory)
{
    startWorker(workingDirectory, QString{});
}

void TerminalBackend::startCommand(const QString &command, const QString &workingDirectory)
{
    startWorker(workingDirectory, command);
}

void TerminalBackend::interrupt()
{
    if (m_worker) {
        writeToPty(QByteArrayLiteral("\x03"));
    }
}

void TerminalBackend::startWorker(const QString &workingDirectory, const QString &command)
{
    if (m_state != SessionState::Created) {
        return; // Already started or closed.
    }

    m_terminal = std::make_unique<Terminal>(m_rows, m_cols, m_remoteHostToken);
    m_terminal->screen().setMaxScrollback(ScrollbackSettings::shared().maxLines());
    m_terminal->setDefaultColors(m_defaultFg, m_defaultBg);
    m_terminal->setCellPixelSize(m_cellPixelWidth, m_cellPixelHeight);
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
    QMetaObject::invokeMethod(
        m_worker.get(),
        &PtyWorker::start,
        Qt::QueuedConnection,
        shell,
        m_cols,
        m_rows,
        workingDirectory,
        m_remoteHostToken,
        command
    );
    m_state = SessionState::Starting;
}

void TerminalBackend::startReplay(int cols, int rows, const QByteArray &bytes)
{
    m_cols = cols;
    m_rows = rows;
    m_terminal = std::make_unique<Terminal>(m_rows, m_cols, m_remoteHostToken);
    m_terminal->screen().setMaxScrollback(ScrollbackSettings::shared().maxLines());
    m_terminal->setDefaultColors(m_defaultFg, m_defaultBg);
    m_terminal->setCellPixelSize(m_cellPixelWidth, m_cellPixelHeight);
    m_state = SessionState::Running;
    onData(bytes);
    emit frameReady();
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
            qWarning(
                "Konsoll: session worker did not stop within %lu ms; deferring session removal",
                kThreadJoinTimeoutMs
            );
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
    if (wasMouseTracking != mouseTracking() || wasMouseMotionTracking != mouseMotionTracking()) {
        emit mouseTrackingChanged();
    }
    if (wasCommandRunning != commandRunning()) {
        emit commandRunningChanged();
    }
    if (m_reportedKeyboardFlags != keyboardFlags()) {
        m_reportedKeyboardFlags = static_cast<unsigned char>(keyboardFlags());
        emit keyboardFlagsChanged();
    }
    // Query replies go out in the order the queries arrived: Neovim pairs
    // OSC 11 with CSI 5n and expects the OSC color reply first.
    flushResponses();
    handleRequests();
    updateAnimationTimer();
    if (m_terminal->colorsDirty()) {
        // OSC 4/10/11/12 changed the palette or default colors.
        m_terminal->clearColorsDirty();
        emit colorSchemeChanged();
    }

    emitTerminalEvents();

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

    // The scrollbar follows at frame rate (onFrameTimer): a signal per chunk
    // re-ran its QML handler thousands of times a second under a burst.
    if (m_terminal->synchronizedUpdates()) {
        m_frameTimer.stop();
    } else {
        scheduleFrame();
    }
    // Acknowledge consumption to the worker so it can resume paused reads.
    if (m_worker) {
        QMetaObject::invokeMethod(
            m_worker.get(),
            &PtyWorker::outputDelivered,
            Qt::QueuedConnection,
            static_cast<qint64>(data.size())
        );
    }
}

void TerminalBackend::emitTerminalEvents()
{
    if (m_terminal->commandFinished()) {
        m_terminal->clearCommandFinished();
        // The command that ran the agent ended, whatever it last reported.
        setAgentStatus(AgentState::None, {}, {});
        emit commandFinished(m_terminal->lastExitCode());
    }
    if (m_terminal->bellPending()) {
        m_terminal->clearBellPending();
        emit bellRang();
    }
    if (m_terminal->progressDirty()) {
        m_terminal->clearProgressDirty();
        emit progressChanged();
    }
    if (m_terminal->titleDirty()) {
        m_title = m_terminal->title();
        emit titleChanged();
        emit displayTitleChanged();
        m_terminal->clearTitleDirty();
    }
    if (m_terminal->currentDirectoryDirty()) {
        m_currentDirectory = m_terminal->currentDirectory();
        emit currentDirectoryChanged();
        emit displayTitleChanged();
        m_terminal->clearCurrentDirectoryDirty();
    }
    if (m_terminal->remoteHostDirty()) {
        m_remoteHost = m_terminal->remoteHost();
        emit remoteHostChanged();
        m_terminal->clearRemoteHostDirty();
        updateProcessKind();
    }
    if (m_terminal->pointerShapeDirty()) {
        m_terminal->clearPointerShapeDirty();
        emit pointerShapeChanged();
    }
    emit outputActivity();
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

void TerminalBackend::onForegroundChanged(const QString &name)
{
    // The worker reports on every poll; cache only on change.
    if (name == m_foregroundProcess) {
        return;
    }
    m_foregroundProcess = name;
    emit foregroundProcessChanged();
    emit displayTitleChanged();
    updateProcessKind();
    // Back at the prompt: an agent that was killed (Ctrl+C, a crash) never
    // reported its end.
    if (!hasForegroundProcess()) {
        setAgentStatus(AgentState::None, {}, {});
    }
}

void TerminalBackend::setAgentStatus(AgentState state, const QString &agent, const QString &message)
{
    const QString name = state == AgentState::None ? QString{} : agent;
    const QString text = state == AgentState::None ? QString{} : message;
    if (state == m_agentState && name == m_agentName && text == m_agentMessage) {
        return;
    }
    if (state != m_agentState) {
        m_agentSince = QDateTime::currentMSecsSinceEpoch();
    }
    if (state == AgentState::None) {
        m_agentTask.clear();
    } else if (state == AgentState::Working && !text.isEmpty() && !isAgentSystemMessage(text)) {
        m_agentTask = text; // a new prompt (UserPromptSubmit)
    }
    m_agentState = state;
    m_agentName = name;
    m_agentMessage = text;
    emit agentStatusChanged();
}

void TerminalBackend::applyRemoteAgentReport(const QByteArray &payload)
{
    if (const auto report = parseRemoteAgentReport(payload, remoteAgentToken(m_remoteHostToken))) {
        setAgentStatus(report->state(), report->agent, report->message);
    }
}

bool TerminalBackend::answerApproval(bool allow)
{
    if (m_readAnswer) {
        answerReadRequest(allow); // Konsoll's own prompt, not the agent's
        return true;
    }
    if (m_agentState != AgentState::Approval || !m_worker) {
        return false;
    }
    writeToPty(allow ? QByteArrayLiteral("\r") : QByteArrayLiteral("\x1b"));
    // Allowed, the tool runs (PostToolUse reports working); refused, the
    // agent stops and waits for the user's next message.
    setAgentStatus(allow ? AgentState::Working : AgentState::Idle, m_agentName, {});
    return true;
}

QString TerminalBackend::displayTitle() const
{
    // "?" is a job whose name could not be read.
    if (hasForegroundProcess() && m_foregroundProcess != QStringLiteral("?")) {
        return m_foregroundProcess;
    }
    return m_currentDirectory.isEmpty() ? m_title : m_currentDirectory;
}

void TerminalBackend::updateProcessKind()
{
    // The shell integration reports the ssh target as the remote host while
    // ssh runs, also when it is started through a wrapper (sudo, a script).
    const ProcessKind kind = !m_remoteHost.isEmpty() && hasForegroundProcess()
                                 ? ProcessKind::Remote
                                 : konsoll::processKind(m_foregroundProcess);
    if (kind != m_processKind) {
        m_processKind = kind;
        emit processKindChanged();
    }
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
    // Output arrived since the timer started: wait out the rest of the quiet
    // period from the last chunk (see scheduleFrame).
    const auto quiet = std::chrono::milliseconds(m_sinceOutput.elapsed());
    if (quiet < kFrameDelay) {
        m_frameTimer.start(kFrameDelay - quiet);
        return;
    }
    checkScrollOffsetChanged();
    if (m_terminal && m_terminal->synchronizedUpdates()) {
        return;
    }
    emit frameReady();
}

void TerminalBackend::updateAnimationTimer()
{
    const bool animating = m_terminal && m_terminal->animating();
    if (animating && !m_animationTimer.isActive()) {
        m_animationTimer.start();
    } else if (!animating && m_animationTimer.isActive()) {
        m_animationTimer.stop();
    }
}

void TerminalBackend::onAnimationTimer()
{
    if (m_terminal && m_terminal->advanceAnimations(m_animationTimer.interval())) {
        scheduleFrame();
    }
    updateAnimationTimer();
}

void TerminalBackend::scheduleFrame()
{
    // Trailing-edge coalescing: restarting the timer on every chunk makes the
    // repaint land ~16ms after the LAST chunk of a multi-chunk PTY update, not
    // the first. A leading-edge start (only when inactive) fires mid-update and
    // paints a torn frame — cleared-but-not-yet-rewritten cells show the desktop
    // through the translucency ("white blocks") and half-shifted rows look skewed.
    // Restarting a QTimer costs a kill and a start of a system timer, though,
    // so a chunk only notes the time and onFrameTimer pushes itself back.
    m_sinceOutput.start();
    if (!m_frameTimer.isActive()) {
        m_frameTimer.start(kFrameDelay);
    }
}

void TerminalBackend::keyPress(int key, const QString &text, int modifiers)
{
    keyEvent(key, text, modifiers, KeyEventType::Press);
}

void TerminalBackend::keyEvent(int key, const QString &text, int modifiers, int eventType)
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
        if (eventType != KeyEventType::Release) {
            editorShortcuts(key);
        }
        return;
    }
    const bool keypad = (mods & static_cast<unsigned int>(Qt::KeypadModifier)) != 0;
    const unsigned int keyMods =
        (shift ? KeyMod::Shift : 0u) | (option ? KeyMod::Alt : 0u) | (control ? KeyMod::Control : 0u);
    if (eventType == KeyEventType::Release) {
        // Releases only reach the PTY under kitty flag 2.
        const QByteArray out = keyOutputBytes(key, text, keyMods, keypad, eventType);
        if (!out.isEmpty() && m_worker) {
            writeToPty(out);
        }
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

    // Physical Ctrl+K: clear the scrollback buffer (terminal-level action,
    // like Konsole's "Clear Buffer"). Not sent to the shell.
    if (control && key == static_cast<int>(Qt::Key_K)) {
        clearBuffer();
        return;
    }

    const QByteArray out = keyOutputBytes(key, text, keyMods, keypad, eventType);
    if (!out.isEmpty() && m_worker) {
        writeToPty(out);
    }
}

QByteArray
TerminalBackend::keyOutputBytes(int key, const QString &text, unsigned int mods, bool keypad, int eventType) const
{
    if (!m_terminal) {
        return {};
    }
    const unsigned int modes =
        (m_terminal->applicationCursorKeys() ? KeyMode::ApplicationCursor : 0u) |
        (m_terminal->applicationKeypad() ? KeyMode::ApplicationKeypad : 0u) | (keypad ? KeyMode::FromKeypad : 0u) |
        (m_terminal->newlineMode() ? KeyMode::NewlineMode : 0u) | (m_terminal->vt52Mode() ? KeyMode::Vt52 : 0u) |
        (m_terminal->modifyOtherKeys() == 1 ? KeyMode::OtherKeys1 : 0u) |
        (m_terminal->modifyOtherKeys() == 2 ? KeyMode::OtherKeys2 : 0u) |
        (m_terminal->otherKeysCsiU() ? KeyMode::OtherKeysCsiU : 0u) | (optionAsMeta() ? KeyMode::OptionMeta : 0u);
    // DECUDK: Shift+F1-F20 send the strings the program defined.
    if (mods == KeyMod::Shift && key >= Qt::Key_F1 && key <= Qt::Key_F20 && eventType == KeyEventType::Press) {
        if (QByteArray defined = m_terminal->userDefinedKey(key - Qt::Key_F1 + 1); !defined.isEmpty()) {
            return defined;
        }
    }
    return encodeKey(
        key,
        text,
        mods,
        modes,
        m_terminal->kittyKeyboardFlags(),
        eventType,
        usLayoutKey(NativeKeyTracker::lastVirtualKey())
    );
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

void TerminalBackend::mouseWheelHorizontal(bool left, int row, int col, int modifiers)
{
    sendMouseReport(left ? 66 : 67, row, col, left ? 5 : 6, modifiers);
}

QVariantMap TerminalBackend::linkAt(int row, int col) const
{
    if (!m_terminal || row < 0 || row >= m_rows || col < 0 || col >= m_cols) {
        return {};
    }
    const QVariantMap hyperlink = hyperlinkAt(row, col);
    if (!hyperlink.isEmpty()) {
        return hyperlink;
    }

    const Screen &screen = m_terminal->screen();
    const Line &cells = screen.lineAt(visibleStart() + row);
    QString line;
    std::ranges::for_each(std::views::iota(0, static_cast<int>(cells.size())), [&cells, &line](int index) {
        // A cluster becomes one placeholder so text positions stay cell columns.
        const unsigned int ch = cells.at(static_cast<std::size_t>(index)).ch;
        const auto codePoint = static_cast<char32_t>(ch >= kClusterBase ? 0xFFFCu : ch);
        line.append(QString::fromUcs4(&codePoint, 1));
    });

    static const QRegularExpression linkPattern(
        QStringLiteral(R"((?:https?|ftp|file)://[^\s<>"']+|mailto:[^\s<>"']+|www\.[^\s<>"']+|(?:~?/|\.\.?/)[^\s<>"']+)")
    );
    auto matches = linkPattern.globalMatch(line);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QVariantMap link = linkFromMatch(match, col);
        if (!link.isEmpty()) {
            return link;
        }
    }
    return {};
}

// An OSC 8 hyperlink under the cell: the run of cells sharing its id on the
// row. Only web, mail and existing local file targets are offered.
QVariantMap TerminalBackend::hyperlinkAt(int row, int col) const
{
    const Line &cells = m_terminal->screen().lineAt(visibleStart() + row);
    if (col >= static_cast<int>(cells.size()) || cells[static_cast<std::size_t>(col)].link == 0) {
        return {};
    }
    const unsigned short id = cells[static_cast<std::size_t>(col)].link;
    const QUrl url(m_terminal->linkTarget(id));
    static const QStringList kSchemes{
        QStringLiteral("http"),
        QStringLiteral("https"),
        QStringLiteral("ftp"),
        QStringLiteral("mailto"),
        QStringLiteral("file")
    };
    if (!url.isValid() || !kSchemes.contains(url.scheme().toLower()) ||
        (url.isLocalFile() && !QFileInfo::exists(url.toLocalFile()))) {
        return {};
    }
    const auto sameLink = [id](const Cell &cell) { return cell.link == id; };
    const auto at = cells.begin() + col;
    const auto first = std::find_if_not(std::make_reverse_iterator(at), cells.rend(), sameLink).base();
    const auto last = std::find_if_not(at, cells.end(), sameLink);
    return {
        {QStringLiteral("target"), url.toString()},
        {QStringLiteral("start"), static_cast<int>(first - cells.begin())},
        {QStringLiteral("end"), static_cast<int>(last - cells.begin())}
    };
}

void TerminalBackend::wheelScroll(int lines)
{
    if (!m_terminal || lines == 0) {
        return;
    }
    // ?1007: full-screen apps without mouse tracking (less, man) get cursor
    // keys, since the alternate screen has no scrollback to show.
    if (m_terminal->screen().inAltScreen() && m_terminal->alternateScroll() && !mouseTracking()) {
        const bool app = m_terminal->applicationCursorKeys();
        const QByteArray arrow = lines > 0 ? (app ? "\x1bOA" : "\x1b[A") : (app ? "\x1bOB" : "\x1b[B");
        if (m_worker) {
            writeToPty(arrow.repeated(std::abs(lines)));
        }
        return;
    }
    scrollBy(lines);
}

QVariantMap TerminalBackend::linkFromMatch(const QRegularExpressionMatch &match, int col) const
{
    if (col < match.capturedStart() || col >= match.capturedEnd()) {
        return {};
    }

    QString target = match.captured();
    while (!target.isEmpty() && QStringLiteral(".,;:!?) ]}").contains(target.back())) {
        target.chop(1);
    }
    if (target.isEmpty()) {
        return {};
    }
    const auto targetLength = target.size();
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
    flushResponses(); // mode 2048 in-band resize report

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
    Line line;
    for (int lineIndex = 0; lineIndex < screen.totalLines(); ++lineIndex) {
        screen.copyLine(lineIndex, line);
        QString text;
        QVector<int> offsetToCell;
        text.reserve(static_cast<qsizetype>(line.size()));
        for (int cellIndex = 0; cellIndex < static_cast<int>(line.size()); ++cellIndex) {
            const Cell &cell = line.at(static_cast<std::size_t>(cellIndex));
            const QString glyph = cellText(cell.ch);
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
    return static_cast<int>(m_searchMatches.size());
}

int TerminalBackend::searchNext(bool backwards)
{
    if (m_searchMatches.isEmpty()) {
        return 0;
    }
    const int count = static_cast<int>(m_searchMatches.size());
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
            appendCellText(text, l[c]);
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
            std::ranges::for_each(std::views::iota(0, end + 1), [&](int col) { appendCellText(text, cells[col]); });
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
        std::ranges::for_each(std::views::iota(first, end), [&](int col) { appendCellText(text, cells[col]); });
        if (line < bot && !screen.isWrappedContinuation(line + 1)) {
            text.append('\n');
        }
    });
    return text;
}

QString TerminalBackend::bufferText() const
{
    return m_terminal ? linesText(0, m_terminal->screen().totalLines() - 1) : QString{};
}

QString TerminalBackend::linesText(int first, int last, int firstColumn) const
{
    if (!m_terminal) {
        return {};
    }
    const Screen &screen = m_terminal->screen();
    const int from = std::max(0, first);
    const int to = std::min(last, screen.totalLines() - 1);
    QString text;
    Line cells;
    std::ranges::for_each(std::views::iota(from, std::max(from, to + 1)), [&](int line) {
        screen.copyLine(line, cells);
        // A line that wraps onto the next keeps its trailing blanks: a space
        // in the last column is part of the text ("false; echo" wrapped
        // after the semicolon), not padding.
        const bool wraps = line < to && screen.isWrappedContinuation(line + 1);
        const auto reversedCells = std::views::reverse(cells);
        const auto lastContent = std::ranges::find_if(reversedCells, [](const Cell &cell) { return cell.ch != ' '; });
        const int end = wraps ? static_cast<int>(cells.size())
                        : lastContent == reversedCells.end()
                            ? 0
                            : static_cast<int>(std::distance(cells.begin(), lastContent.base()));
        const int begin = line == from ? std::min(firstColumn, end) : 0;
        std::ranges::for_each(std::views::iota(begin, end), [&](int col) { appendCellText(text, cells[col]); });
        if (line < to && !wraps) {
            text.append('\n');
        }
    });
    return text;
}

QJsonArray TerminalBackend::recentCommands(int count, qsizetype outputLimit) const
{
    QJsonArray commands;
    if (!m_terminal) {
        return commands;
    }
    const auto &blocks = m_terminal->commandBlocks();
    const auto recent = blocks | std::views::drop(std::max(0, static_cast<int>(blocks.size()) - count));
    std::ranges::for_each(recent, [this, &commands, outputLimit](const Terminal::CommandBlock &block) {
        // The typed command runs from after the prompt to the line its
        // output starts on.
        const int commandEnd = std::max(block.startLine, block.outputLine - 1);
        const QString command = linesText(block.startLine, commandEnd, block.startColumn).trimmed();
        const QString output = block.endLine >= block.outputLine
                                   ? linesText(block.outputLine, block.endLine).trimmed().right(outputLimit)
                                   : QString{};
        commands.append(QJsonObject{
            {QStringLiteral("command"), command},
            {QStringLiteral("exitCode"), block.exitCode},
            {QStringLiteral("output"), output},
        });
    });
    return commands;
}

bool TerminalBackend::requestRead(const QString &description, std::function<void(bool allow)> answer)
{
    if (m_readAnswer) {
        return false;
    }
    m_readRequest = description;
    m_readAnswer = std::move(answer);
    emit readRequestChanged();
    // Like a permission prompt: the tab, the notification (whose Allow / Deny
    // answer this request, answerApproval) and the agents list show it.
    setAgentStatus(AgentState::Approval, m_agentName.isEmpty() ? m_foregroundProcess : m_agentName, description);
    return true;
}

void TerminalBackend::answerReadRequest(bool allow)
{
    if (!m_readAnswer) {
        return;
    }
    const auto answer = std::exchange(m_readAnswer, {});
    m_readRequest.clear();
    emit readRequestChanged();
    if (m_agentState == AgentState::Approval) {
        setAgentStatus(AgentState::Working, m_agentName, {}); // the tool call goes on
    }
    answer(allow);
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

void TerminalBackend::pasteText(const QString &text)
{
    if (!m_worker || text.isEmpty()) {
        return;
    }
    QByteArray pasted = text.toUtf8();
    pasted.replace('\x1b', QByteArray{});
    if (m_terminal && m_terminal->bracketedPasteEnabled()) {
        pasted.prepend("\x1b[200~");
        pasted.append("\x1b[201~");
    }
    writeToPty(pasted);
}

QString TerminalBackend::lastCommandText() const
{
    const QJsonArray commands = recentCommands(1, kSentOutputLength);
    if (commands.isEmpty()) {
        return {};
    }
    const QJsonObject command = commands.first().toObject();
    const QString output = command.value(QLatin1String("output")).toString();
    return tr("$ %1\n(exit status %2)\n%3")
        .arg(command.value(QLatin1String("command")).toString())
        .arg(command.value(QLatin1String("exitCode")).toInt(-1))
        .arg(output);
}

void TerminalBackend::paste()
{
    if (!m_worker) {
        return;
    }
    const QClipboard *const clipboard = QGuiApplication::clipboard();
    const QImage image = clipboard->image();
    if (!image.isNull() && !m_remoteHost.isEmpty() && m_remoteHost != QStringLiteral("localhost")) {
        transferImage(image);
    } else if (!image.isNull()) {
        // Image-aware terminal applications (including Codex CLI) handle Ctrl+V
        // themselves and read the native macOS clipboard. A PTY cannot carry an
        // image object, so forward the application's image-paste key instead of
        // converting the image to a temporary path.
        writeToPty(QByteArray(1, '\x16'));
    } else {
        const QString text = clipboard->text();
        if (!text.isEmpty()) {
            QByteArray pastedText = text.toUtf8();
            if (m_terminal && m_terminal->bracketedPasteEnabled()) {
                pastedText.prepend("\x1b[200~");
                pastedText.append("\x1b[201~");
            }
            writeToPty(pastedText);
        }
    }
    clearTextSelectionAndHighlight();
}

void TerminalBackend::transferFile(const QUrl &url)
{
    if (!url.isLocalFile()) {
        return;
    }
    transferLocalFile(url.toLocalFile());
}

void TerminalBackend::transferImage(const QImage &image)
{
    const QString localPath =
        QDir::temp().filePath(QStringLiteral("konsoll-%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!image.save(localPath, "PNG")) {
        qWarning("Konsoll: failed to encode clipboard image");
        emit transferFailed(tr("Could not encode the clipboard image."));
        return;
    }
    transferLocalFile(localPath, true);
}

void TerminalBackend::transferLocalFile(const QString &path, bool removeAfterTransfer)
{
    if (!m_worker) {
        if (removeAfterTransfer) {
            QFile::remove(path);
        }
        return;
    }
    const QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        if (removeAfterTransfer) {
            QFile::remove(path);
        }
        return;
    }
    if (m_remoteHost.isEmpty() || m_remoteHost == QStringLiteral("localhost")) {
        writeToPty(path.toUtf8());
        if (removeAfterTransfer) {
            QFile::remove(path);
        }
        return;
    }

    static const QRegularExpression remoteSpec(
        QStringLiteral("^(?:(?<user>[A-Za-z0-9_.-]+)@)?(?<host>[A-Za-z0-9._-]+)(?:;port=(?<port>[0-9]{1,5}))?$")
    );
    const QRegularExpressionMatch remoteMatch = remoteSpec.match(m_remoteHost);
    if (!remoteMatch.hasMatch()) {
        qWarning() << "Konsoll: ignoring unsupported remote host in terminal metadata";
        emit transferFailed(tr("Could not identify the SSH host for this transfer."));
        if (removeAfterTransfer) {
            QFile::remove(path);
        }
        return;
    }
    const QString suffix = fileInfo.isDir() ? QString{} : fileInfo.completeSuffix();
    const QString remoteName = QStringLiteral("konsoll-%1%2")
                                   .arg(
                                       QUuid::createUuid().toString(QUuid::WithoutBraces),
                                       suffix.isEmpty() ? QString{} : QStringLiteral(".") + suffix
                                   );
    const QString remotePath = QStringLiteral("/tmp/") + remoteName;
    const QString port = remoteMatch.captured(QStringLiteral("port"));
    if (!port.isEmpty() && (port.toUInt() == 0 || port.toUInt() > 65535)) {
        qWarning() << "Konsoll: ignoring invalid SSH port in terminal metadata";
        emit transferFailed(tr("The SSH port reported by the shell is invalid."));
        if (removeAfterTransfer) {
            QFile::remove(path);
        }
        return;
    }
    const QString user = remoteMatch.captured(QStringLiteral("user"));
    const QString host = remoteMatch.captured(QStringLiteral("host"));
    const QString destination = (user.isEmpty() ? QString{} : user + QLatin1Char('@')) + host;
    const TransferJob job{
        .localPath = fileInfo.absoluteFilePath(),
        .remotePath = remotePath,
        .destination = destination,
        .port = port,
        .targetHost = m_remoteHost,
        .flags = removeAfterTransfer ? 1u : 0u,
    };
    if (m_transferProcess.state() != QProcess::NotRunning || !m_transferQueue.isEmpty()) {
        m_transferQueue.enqueue(job);
        return;
    }
    m_transferQueue.enqueue(job);
    startNextTransfer();
}

void TerminalBackend::startNextTransfer()
{
    if (m_transferProcess.state() != QProcess::NotRunning || m_transferQueue.isEmpty()) {
        return;
    }
    while (!m_transferQueue.isEmpty()) {
        m_activeTransfer = m_transferQueue.dequeue();
        if (m_activeTransfer.targetHost == m_remoteHost) {
            break;
        }
        if (m_activeTransfer.removeLocal()) {
            QFile::remove(m_activeTransfer.localPath);
        }
        m_activeTransfer = {};
    }
    if (m_activeTransfer.localPath.isEmpty()) {
        return;
    }
    QStringList arguments{QStringLiteral("-q")};
    if (!m_activeTransfer.port.isEmpty()) {
        arguments.append(QStringLiteral("-P"));
        arguments.append(m_activeTransfer.port);
    }
    if (QFileInfo(m_activeTransfer.localPath).isDir()) {
        arguments.append(QStringLiteral("-r"));
    }
    arguments.append(QStringLiteral("-o"));
    arguments.append(QStringLiteral("BatchMode=yes"));
    arguments.append(QStringLiteral("-o"));
    arguments.append(QStringLiteral("ConnectTimeout=10"));
    arguments.append(QStringLiteral("--"));
    arguments.append(m_activeTransfer.localPath);
    arguments.append(m_activeTransfer.destination + QLatin1Char(':') + m_activeTransfer.remotePath);
    m_transferProcess.start(QStringLiteral("scp"), arguments);
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

Cell TerminalBackend::resolvedCell(const Cell &cell) const
{
    // DECSCNM (reverse video) swaps the default colors for the whole screen.
    const bool reverseScreen = m_terminal && m_terminal->reverseVideo();
    const unsigned int defaultFg = foregroundColor();
    const unsigned int defaultBg = backgroundColor();
    unsigned int fg = resolveColor(cell.fg, reverseScreen ? defaultBg : defaultFg);
    unsigned int bg = resolveColor(cell.bg, reverseScreen ? defaultFg : defaultBg);
    if ((cell.flags & CellFlag::Reverse) != 0) {
        std::swap(fg, bg);
    }
    fg = readableForeground(static_cast<QRgb>(fg), static_cast<QRgb>(bg));
    if ((cell.flags & CellFlag::Dim) != 0) {
        // SGR 2 (faint): halfway to the background, as in iTerm2 and VTE.
        const auto mix = [](int a, int b) { return (a + b) / 2; };
        fg = qRgb(mix(qRed(fg), qRed(bg)), mix(qGreen(fg), qGreen(bg)), mix(qBlue(fg), qBlue(bg)));
    }
    Cell resolved = cell;
    resolved.fg = fg;
    resolved.bg = bg;
    // SGR 58 underline color; 0 follows the (resolved) text color.
    resolved.ul = cell.ul == 0 ? fg : resolveColor(cell.ul, fg);
    return resolved;
}

void TerminalBackend::visibleRowCells(int row, std::vector<Cell> &out) const
{
    out.clear();
    if (!m_terminal || row < 0 || row >= gridRows()) {
        return;
    }
    // The visible window is the last `rows` lines, shifted up by the scroll
    // offset (0 = at the bottom). Short lines are padded with blank cells.
    m_terminal->screen().copyLine(visibleStart() + row, out);
    out.resize(static_cast<std::size_t>(gridCols()));
    std::ranges::transform(out, out.begin(), [this](const Cell &cell) { return resolvedCell(cell); });
}

QVariantList TerminalBackend::snapshot() const
{
    QVariantList rowsOut;
    if (!m_terminal) {
        return rowsOut;
    }
    rowsOut.reserve(static_cast<qsizetype>(gridRows()) * gridCols());
    std::vector<Cell> cells;
    std::ranges::for_each(std::views::iota(0, gridRows()), [&](int r) {
        visibleRowCells(r, cells);
        std::ranges::for_each(cells, [&rowsOut](const Cell &cell) {
            // Encode the code point as a QString (surrogate pair when
            // supplementary) so the QML renderer draws one glyph per cell.
            QVariantMap m;
            m["ch"] = cellText(cell.ch);
            m["fg"] = static_cast<int>(cell.fg);
            m["bg"] = static_cast<int>(cell.bg);
            m["flags"] = static_cast<int>(cell.flags);
            rowsOut.append(m);
        });
    });
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

unsigned int TerminalBackend::backgroundColor() const
{
    return m_terminal && m_terminal->backgroundOverride() != 0 ? m_terminal->backgroundOverride() : m_defaultBg;
}

unsigned int TerminalBackend::foregroundColor() const
{
    return m_terminal && m_terminal->foregroundOverride() != 0 ? m_terminal->foregroundOverride() : m_defaultFg;
}

unsigned int TerminalBackend::cursorColor() const
{
    return m_terminal && m_terminal->cursorColorOverride() != 0 ? m_terminal->cursorColorOverride() : foregroundColor();
}

std::vector<TerminalBackend::VisibleImage> TerminalBackend::visibleImages() const
{
    std::vector<VisibleImage> out;
    if (!m_terminal) {
        return out;
    }
    const Screen &screen = m_terminal->screen();
    const GraphicsStore &graphics = m_terminal->graphics();
    const long long top = screen.lineAnchor(visibleStart());
    const int alternate = screen.inAltScreen() ? 1 : 0;
    const double cellWidth = std::max(m_cellPixelWidth, 1);
    const double cellHeight = std::max(m_cellPixelHeight, 1);
    std::ranges::for_each(graphics.placements(), [&](const GraphicsPlacement &placement) {
        const long long row = placement.anchor - top;
        const GraphicsImage *const image = graphics.image(placement.imageId);
        if (placement.alternate != alternate || image == nullptr || row + placement.rows <= 0 || row >= m_rows ||
            placement.scaled == kPlacementVirtual) {
            return;
        }
        // c= / r= stretch the image over that many cells; otherwise it keeps
        // its pixel size.
        const bool scaled = placement.scaled == kPlacementScaled;
        const QRectF target(
            placement.column + (placement.offsetX / cellWidth),
            static_cast<double>(row) + (placement.offsetY / cellHeight),
            scaled ? placement.columns : placement.source.width() / cellWidth,
            scaled ? placement.rows : placement.source.height() / cellHeight
        );
        out.push_back(VisibleImage{
            .image = image->pixels,
            .target = target,
            .source = QRectF(placement.source),
            .serial = image->serial,
            .z = placement.z,
        });
    });
    appendPlaceholderImages(out);
    std::ranges::stable_sort(out, {}, &VisibleImage::z);
    return out;
}

namespace
{
// A cell color as a kitty placeholder id: 256-color index or 24-bit RGB.
std::uint32_t placeholderColorId(unsigned int encoded)
{
    if (encoded >= 1 && encoded <= 256) {
        return encoded - 1;
    }
    return (encoded & 0x80000000u) != 0 ? (encoded & 0xffffffu) : 0u;
}

// One kitty placeholder cell: image / placement id and the image cell it
// shows (row, column; -1 when the diacritic is absent).
struct PlaceholderCell {
    std::uint32_t image{0};
    std::uint32_t placement{0};
    int row{-1};
    int column{-1};
};

PlaceholderCell decodePlaceholder(const Cell &cell)
{
    const std::u32string codePoints = cellCodePoints(cell.ch);
    PlaceholderCell out;
    if (codePoints.empty() || codePoints.front() != kImagePlaceholder) {
        return out;
    }
    out.image = placeholderColorId(cell.fg);
    out.placement = placeholderColorId(cell.ul);
    const auto diacritic = [&codePoints](std::size_t i) {
        return i < codePoints.size() ? placeholderIndex(codePoints[i]) : -1;
    };
    out.row = diacritic(1);
    out.column = diacritic(2);
    if (const int high = diacritic(3); high >= 0) {
        out.image |= static_cast<std::uint32_t>(high) << 24u; // most significant byte
    }
    return out;
}
} // namespace

namespace
{
// Extends `last` with `next` when it continues it on the same row.
bool mergeSlice(TerminalBackend::VisibleImage &last, const TerminalBackend::VisibleImage &next)
{
    if (last.serial != next.serial || !qFuzzyCompare(last.target.right(), next.target.left()) ||
        !qFuzzyCompare(last.target.top(), next.target.top()) ||
        !qFuzzyCompare(last.source.right(), next.source.left())) {
        return false;
    }
    last.target.setRight(next.target.right());
    last.source.setRight(next.source.right());
    return true;
}
} // namespace

// The part of a virtual placement one placeholder cell shows: the image
// fitted into the placement's c x r cells (aspect kept, centered).
std::optional<TerminalBackend::VisibleImage> TerminalBackend::placeholderSlice(
    std::uint32_t imageId,
    std::uint32_t placementId,
    int imageRow,
    int imageColumn,
    int row,
    int col
) const
{
    const GraphicsStore &graphics = m_terminal->graphics();
    const GraphicsPlacement *const placement = graphics.virtualPlacement(imageId, placementId);
    const GraphicsImage *const image = placement != nullptr ? graphics.image(imageId) : nullptr;
    if (image == nullptr) {
        return std::nullopt;
    }
    const double cellWidth = std::max(m_cellPixelWidth, 1);
    const double cellHeight = std::max(m_cellPixelHeight, 1);
    const QRectF source(placement->source);
    const double boxWidth = std::max(placement->columns, 1) * cellWidth;
    const double boxHeight = std::max(placement->rows, 1) * cellHeight;
    const double scale = std::min(boxWidth / source.width(), boxHeight / source.height());
    const QRectF drawn(
        (boxWidth - (source.width() * scale)) / 2.0,
        (boxHeight - (source.height() * scale)) / 2.0,
        source.width() * scale,
        source.height() * scale
    );
    const QRectF cell(imageColumn * cellWidth, imageRow * cellHeight, cellWidth, cellHeight);
    const QRectF piece = cell.intersected(drawn);
    if (piece.isEmpty()) {
        return std::nullopt;
    }
    return VisibleImage{
        .image = image->pixels,
        .target = QRectF(
            col + ((piece.x() - cell.x()) / cellWidth),
            row + ((piece.y() - cell.y()) / cellHeight),
            piece.width() / cellWidth,
            piece.height() / cellHeight
        ),
        .source = QRectF(
            source.x() + ((piece.x() - drawn.x()) / scale),
            source.y() + ((piece.y() - drawn.y()) / scale),
            piece.width() / scale,
            piece.height() / scale
        ),
        .serial = image->serial,
        .z = placement->z,
    };
}

// Kitty Unicode placeholders (U+10EEEE): each cell shows one cell of a
// virtual placement's grid. Missing diacritics continue the previous cell;
// consecutive cells of one image row become one rectangle.
void TerminalBackend::appendPlaceholderImages(std::vector<VisibleImage> &out) const
{
    Line line;
    std::ranges::for_each(std::views::iota(0, m_rows), [&](int row) {
        m_terminal->screen().copyLine(visibleStart() + row, line);
        PlaceholderCell previous;
        int previousCol = -2;
        std::ranges::for_each(std::views::iota(0, static_cast<int>(line.size())), [&](int col) {
            PlaceholderCell cell = decodePlaceholder(line[static_cast<std::size_t>(col)]);
            if (cell.image == 0) {
                previousCol = -2;
                return;
            }
            const bool continues = previousCol == col - 1 && previous.image == cell.image;
            cell.row = cell.row >= 0 ? cell.row : (continues ? previous.row : 0);
            cell.column = cell.column >= 0 ? cell.column : (continues ? previous.column + 1 : 0);
            previous = cell;
            previousCol = col;
            const std::optional<VisibleImage> slice =
                placeholderSlice(cell.image, cell.placement, cell.row, cell.column, row, col);
            if (slice && (out.empty() || !mergeSlice(out.back(), *slice))) {
                out.push_back(*slice);
            }
        });
    });
}

// Settings > Keyboard "Use Option as Meta", or ?1036 / ?1039 from the app.
bool TerminalBackend::optionAsMeta() const
{
    return (m_terminal && m_terminal->metaSendsEscape()) ||
           QSettings().value(QStringLiteral("General/optionAsMeta"), false).toBool();
}

int TerminalBackend::visibleRowSize(int row) const
{
    return m_terminal ? m_terminal->screen().lineSize(visibleStart() + row) : 0;
}

int TerminalBackend::keyboardFlags() const
{
    return m_terminal ? static_cast<int>(m_terminal->kittyKeyboardFlags()) : 0;
}

int TerminalBackend::pointerShape() const
{
    const QByteArray name = m_terminal ? m_terminal->pointerShape() : QByteArray{};
    return name.isEmpty() ? -1 : pointerCursorShape(name);
}

int TerminalBackend::cursorStyle() const
{
    return m_terminal ? m_terminal->cursorStyle() : 0;
}

int TerminalBackend::cursorShape() const
{
    return m_terminal ? m_terminal->cursorShape() : 0;
}

bool TerminalBackend::cursorBlinking() const
{
    return m_terminal && m_terminal->cursorBlinking();
}

void TerminalBackend::setFocused(bool focused)
{
    if (m_focused == focused) {
        return;
    }
    m_focused = focused;
    if (m_terminal && m_terminal->focusReporting() && m_worker) {
        writeToPty(focused ? QByteArray("\x1b[I") : QByteArray("\x1b[O"));
    }
    scheduleFrame();
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

QRgb TerminalBackend::resolveColor(unsigned int encoded, unsigned int defaultColor) const
{
    if (encoded == 0) {
        return static_cast<QRgb>(defaultColor);
    }
    if (encoded <= 256) {
        // The palette, with any OSC 4 overrides the application set.
        const int index = static_cast<int>(encoded) - 1;
        return static_cast<QRgb>(m_terminal ? m_terminal->paletteColor(index) : defaultPaletteColor(index));
    }
    if ((encoded & 0x80000000u) != 0u) {
        const unsigned int rgb = encoded & 0xffffff;
        return static_cast<QRgb>(0xff000000u | rgb);
    }
    return static_cast<QRgb>(defaultColor);
}

} // namespace konsoll
