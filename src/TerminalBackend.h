#pragma once

#include "pty/PtyWorker.h"
#include "term/Terminal.h"

#include <QColor>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml>

#include <cstdint>
#include <memory>
#include <vector>

class QImage;
class QRegularExpressionMatch;

namespace konsoll
{

// The QML<->C++ bridge. Owns the session worker (PTY) and the Terminal
// (screen model), feeds parsed output, coalesces repaints to ~60fps, and
// exposes a snapshot to QML.
class TerminalBackend : public QObject
{
    Q_OBJECT

    // Session lifecycle (docs/SESSION-THREADING-PLAN.md §7).
    enum class SessionState : std::uint8_t
    {
        Created,
        Starting,
        Running,
        Failed,
        Stopping,
        Stopped
    };

    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString currentDirectory READ currentDirectory NOTIFY currentDirectoryChanged)
    Q_PROPERTY(QString remoteHost READ remoteHost NOTIFY remoteHostChanged)
    Q_PROPERTY(int rows READ rows NOTIFY sizeChanged)
    Q_PROPERTY(int cols READ cols NOTIFY sizeChanged)
    Q_PROPERTY(int scrollOffset READ scrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(int maxScrollOffset READ maxScrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(bool selectionActive READ selectionActive NOTIFY selectionChanged)
    Q_PROPERTY(bool highlightActive READ highlightActive NOTIFY highlightChanged)
    Q_PROPERTY(int highlightStartRow READ highlightStartRow NOTIFY highlightChanged)
    Q_PROPERTY(int highlightEndRow READ highlightEndRow NOTIFY highlightChanged)
    Q_PROPERTY(bool mouseTracking READ mouseTracking NOTIFY mouseTrackingChanged)
    // Kitty keyboard protocol flags the application enabled: the view sends
    // every key here (flag 8) and key releases (flag 2).
    Q_PROPERTY(int keyboardFlags READ keyboardFlags NOTIFY keyboardFlagsChanged)
    Q_PROPERTY(bool mouseMotionTracking READ mouseMotionTracking NOTIFY mouseTrackingChanged)
    Q_PROPERTY(bool commandRunning READ commandRunning NOTIFY commandRunningChanged)
    Q_PROPERTY(unsigned int backgroundColor READ backgroundColor NOTIFY colorSchemeChanged)
    Q_PROPERTY(unsigned int foregroundColor READ foregroundColor NOTIFY colorSchemeChanged)

public:
    explicit TerminalBackend(QObject *parent = nullptr);
    ~TerminalBackend() override;

    // Start the terminal with the detected default shell.
    Q_INVOKABLE void start(const QString &workingDirectory = {});
    // A session without a PTY that just shows `bytes` (render-dump harness).
    void startReplay(int cols, int rows, const QByteArray &bytes);
    // Request session shutdown. Returns false if the worker thread has not
    // stopped within the bounded wait; objects remain alive until it does.
    bool close();
    Q_INVOKABLE void setSystemColors(const QColor &background, const QColor &foreground);
    // Cell size in (logical) pixels, for XTWINOPS and in-band size reports.
    Q_INVOKABLE void setCellPixelSize(int width, int height);

    // Input from QML.
    Q_INVOKABLE void keyPress(int key, const QString &text, int modifiers);
    // A key press (1), auto-repeat (2) or release (3) — KeyEventType.
    Q_INVOKABLE void keyEvent(int key, const QString &text, int modifiers, int eventType);
    int keyboardFlags() const;
    Q_INVOKABLE void writeText(const QString &text);
    Q_INVOKABLE void mousePress(int button, int row, int col, int modifiers);
    Q_INVOKABLE void mouseRelease(int button, int row, int col, int modifiers);
    Q_INVOKABLE void mouseMove(int button, int row, int col, int modifiers);
    Q_INVOKABLE void mouseWheel(bool up, int row, int col, int modifiers);
    // Wheel without mouse tracking: scroll the history, or send cursor keys
    // in the alternate screen (?1007).
    Q_INVOKABLE void wheelScroll(int lines);
    Q_INVOKABLE QVariantMap linkAt(int row, int col) const;
    Q_INVOKABLE bool openLinkAt(int row, int col) const;
    Q_INVOKABLE void transferFile(const QUrl &url);

    // Resize the terminal (cols x rows), driven by the QML view.
    Q_INVOKABLE void resize(int cols, int rows);

    // Scroll the view up/down by `delta` lines (positive = up).
    Q_INVOKABLE void scrollBy(int delta);
    // Set the absolute scroll offset (lines up from the bottom; clamped).
    Q_INVOKABLE void scrollTo(int offset);
    // Jump to the bottom (newest content).
    Q_INVOKABLE void scrollToBottom();

    // Send Ctrl+L to the shell so it clears the visible screen and redraws.
    Q_INVOKABLE void clearScreen();
    // Clear the screen and the scrollback buffer (Ctrl+L plus Konsole's
    // "Clear Buffer"). Terminal-level action.
    Q_INVOKABLE void clearBuffer();
    Q_INVOKABLE int search(const QString &query, bool regex);
    Q_INVOKABLE int searchNext(bool backwards = false);
    Q_INVOKABLE int searchCurrentIndex() const;
    Q_INVOKABLE void clearSearch();
    Q_INVOKABLE int searchRowLeft(int visibleRow) const;
    Q_INVOKABLE int searchRowRight(int visibleRow) const;

    // Mouse selection, in visible-grid coordinates (row 0 = top of the view).
    Q_INVOKABLE void selPress(int row, int col);
    Q_INVOKABLE void selMove(int row, int col);
    Q_INVOKABLE void selRelease();
    Q_INVOKABLE void selClear();
    Q_INVOKABLE void selWord(int row, int col);
    Q_INVOKABLE void selLine(int row);
    Q_INVOKABLE void selectAll();

    // A click highlights a complete OSC 133 command block when available,
    // otherwise the clicked line. Cmd+C / Copy copies the highlighted text.
    Q_INVOKABLE void highlightLine(int row);
    Q_INVOKABLE void highlightCommandAt(int row);
    Q_INVOKABLE void clearHighlight();
    bool highlightActive() const { return m_highlightStart >= 0; }
    // The highlighted absolute line, or -1 when there is no highlight.
    int highlightedLine() const { return m_highlightStart; }
    // Last non-space column of the highlighted line (-1 when none).
    int highlightedLineEnd() const;
    // Visible row of the highlighted line, or -1 when it is out of view.
    Q_INVOKABLE int highlightVisibleRow() const;
    int highlightStartRow() const { return highlightVisibleStartRow(); }
    int highlightEndRow() const { return highlightVisibleEndRow(); }

    // Per visible row, the selected column range (iTerm2-style): the start
    // line is clipped at the press column, middle lines span the full text,
    // the end line is clipped at the release column. -1 when the row is not
    // part of the selection.
    Q_INVOKABLE int selRowLeft(int visibleRow) const;
    Q_INVOKABLE int selRowRight(int visibleRow) const;

    // Clipboard.
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void pasteSelectionOrClipboard();
    Q_INVOKABLE bool saveTextToFile(const QUrl &url, bool selectionOnly) const;
    QString bufferText() const;
    QString selectionText() const;

    // One-frame snapshot of the visible grid for the QML renderer.
    Q_INVOKABLE QVariantList snapshot() const;
    // Native renderer access (TerminalRenderer). `out` receives exactly
    // gridCols() cells of visible row `row` with colors already resolved to
    // final QRgb values (palette, reverse video and contrast applied).
    void visibleRowCells(int row, std::vector<Cell> &out) const;
    int gridRows() const { return m_terminal ? m_terminal->screen().rows() : 0; }
    int gridCols() const { return m_terminal ? m_terminal->screen().cols() : 0; }
    Q_INVOKABLE int cursorRow() const;
    Q_INVOKABLE int cursorCol() const;
    Q_INVOKABLE bool cursorVisible() const;
    // DECSCUSR shape (0 block, 1 underline, 2 bar) and blinking.
    int cursorShape() const;
    bool cursorBlinking() const;
    // Images placed on the visible rows: `target` in cell units (fractions
    // for pixel offsets and natural sizes), `source` in image pixels.
    struct VisibleImage {
        std::shared_ptr<const QImage> image;
        QRectF target;
        QRect source;
        std::uint64_t serial{0};
        std::int64_t z{0}; // kitty z-index: below the text when negative
    };
    std::vector<VisibleImage> visibleImages() const;
    // DEC line size of a visible row (Screen::SingleLine ... DoubleHeightBottom).
    Q_INVOKABLE int visibleRowSize(int row) const;

    // Keyboard focus of this session's view (window active + view focused):
    // drives the hollow unfocused cursor and ?1004 focus reports.
    Q_INVOKABLE void setFocused(bool focused);
    bool focused() const { return m_focused; }

    QString title() const { return m_title; }
    QString currentDirectory() const { return m_currentDirectory; }
    QString remoteHost() const { return m_remoteHost; }
    int rows() const { return m_rows; }
    int cols() const { return m_cols; }
    int scrollOffset() const { return m_terminal ? m_terminal->screen().scrollOffset() : 0; }
    int maxScrollOffset() const { return m_terminal ? m_terminal->screen().maxScrollOffset() : 0; }
    bool selectionActive() const { return m_selAnchor.line >= 0; }
    bool mouseTracking() const { return m_terminal && m_terminal->mouseTracking(); }
    bool mouseMotionTracking() const { return m_terminal && m_terminal->mouseMotionTracking(); }
    bool commandRunning() const { return m_terminal && m_terminal->commandRunning(); }
    bool hasForegroundProcess() const { return m_hasForeground; }
    // The default colors, or the ones the application set (OSC 10/11/12).
    unsigned int backgroundColor() const;
    unsigned int foregroundColor() const;
    unsigned int cursorColor() const;

signals:
    void frameReady();
    void titleChanged();
    void currentDirectoryChanged();
    void remoteHostChanged();
    void colorSchemeChanged();
    void processExited(int code);
    void startFailed(const QString &error);
    void workerThreadStopped();
    void sizeChanged();
    void scrollOffsetChanged();
    void selectionChanged();
    void highlightChanged();
    void mouseTrackingChanged();
    void keyboardFlagsChanged();
    // XTWINOPS window manipulation the application asked for and the user
    // allows (Settings > Security): operation 1-10 with its parameters.
    void windowOperationRequested(int operation, int first, int second);
    void commandRunningChanged();
    void quitRequested();
    void commandFinished();
    void transferFailed(const QString &message);

private:
    void onData(const QByteArray &data);
    void onStarted();
    void onExited(int code);
    void onStartFailed(const QString &error);
    void onWorkerThreadStopped();
    void onForegroundChanged(bool hasForeground);
    void onFrameTimer();
    void scheduleFrame();
    // Write the terminal's pending query replies and reports to the PTY.
    void flushResponses();
    // Clipboard reads, title reports and window operations, by the user's
    // Settings > Security choices.
    void handleRequests();
    void answerClipboardRead(const QByteArray &selection);
    void checkScrollOffsetChanged();
    int visibleStart() const;
    // Last non-space column of an absolute line (full width if the line is blank).
    int lineTextEnd(int line) const;
    int highlightVisibleStartRow() const;
    int highlightVisibleEndRow() const;

    void editorShortcuts(int key);
    void transferLocalFile(const QString &path, bool removeAfterTransfer = false);
    void transferImage(const QImage &image);
    void startNextTransfer();
    void clearTextSelectionAndHighlight();
    void sendMouseReport(int button, int row, int col, int event, int modifiers);
    QByteArray keyOutputBytes(int key, const QString &text, unsigned int mods, bool keypad, int eventType) const;
    QVariantMap linkFromMatch(const QRegularExpressionMatch &match, int col) const;
    // Route PTY I/O to the session worker via a queued call (never touch the
    // master descriptor from this thread).
    void writeToPty(const QByteArray &data);
    void resizePty(int columns, int rows);

    Cell resolvedCell(const Cell &cell) const;

    static QString detectDefaultShell();
    QRgb resolveColor(unsigned int encoded, unsigned int defaultColor) const;
    QVariantMap hyperlinkAt(int row, int col) const;

    // Selection anchors, in absolute line+col (0 = oldest line). A line < 0
    // marks "no selection". Kept stable across output so a selection stays on
    // the same text while scrolling.
    struct AbsPos {
        int line{0};
        int col{0};
    };

    struct SearchMatch {
        int line;
        int start;
        int end;
    };

    struct TransferJob {
        QString localPath;
        QString remotePath;
        QString destination;
        QString port;
        QString targetHost;
        std::uint64_t flags{0};

        bool removeLocal() const { return (flags & 1u) != 0; }
    };

    std::unique_ptr<QThread> m_thread;
    std::unique_ptr<PtyWorker> m_worker;
    std::unique_ptr<Terminal> m_terminal;
    QProcess m_transferProcess;
    QVector<SearchMatch> m_searchMatches;
    QQueue<TransferJob> m_transferQueue;
    TransferJob m_activeTransfer;

    QTimer m_frameTimer;
    QString m_title{QStringLiteral("Konsoll")};
    QString m_currentDirectory;
    QString m_remoteHost;
    QString m_remoteHostToken;
    AbsPos m_selAnchor{.line = -1, .col = 0};
    AbsPos m_selCurrent{.line = -1, .col = 0};
    int m_highlightStart{-1};
    int m_highlightEnd{-1};
    int m_searchIndex{-1};

    int m_rows{24};
    int m_cols{80};
    int m_cellPixelWidth{8};
    int m_cellPixelHeight{16};

    unsigned int m_defaultFg{0xffe0e0e0};
    unsigned int m_defaultBg{0xff141414};
    bool m_hasForeground{false};
    bool m_focused{true};
    SessionState m_state{SessionState::Created};
};

} // namespace konsoll
