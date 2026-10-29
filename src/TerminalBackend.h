#pragma once

#include "pty/PtyProcess.h"
#include "term/Terminal.h"

#include <QColor>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml>

#include <memory>

namespace konsoll
{

// The QML<->C++ bridge. Owns the PTY and the Terminal (screen model), feeds
// parsed output, coalesces repaints to ~60fps, and exposes a snapshot to QML.
class TerminalBackend : public QObject
{
    Q_OBJECT

    QML_NAMED_ELEMENT(Backend)
    QML_SINGLETON

    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int rows READ rows NOTIFY sizeChanged)
    Q_PROPERTY(int cols READ cols NOTIFY sizeChanged)
    Q_PROPERTY(int scrollOffset READ scrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(int maxScrollOffset READ maxScrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(bool selectionActive READ selectionActive NOTIFY selectionChanged)
    Q_PROPERTY(bool mouseTracking READ mouseTracking NOTIFY mouseTrackingChanged)
    Q_PROPERTY(bool mouseMotionTracking READ mouseMotionTracking NOTIFY mouseTrackingChanged)
    Q_PROPERTY(unsigned int backgroundColor READ backgroundColor CONSTANT)
    Q_PROPERTY(unsigned int foregroundColor READ foregroundColor CONSTANT)

public:
    explicit TerminalBackend(QObject *parent = nullptr);

    // Start the terminal with the detected default shell.
    Q_INVOKABLE void start();

    // Input from QML.
    Q_INVOKABLE void keyPress(int key, const QString &text, int modifiers);
    Q_INVOKABLE void writeText(const QString &text);
    Q_INVOKABLE void mousePress(int button, int row, int col, int modifiers);
    Q_INVOKABLE void mouseRelease(int button, int row, int col, int modifiers);
    Q_INVOKABLE void mouseMove(int button, int row, int col, int modifiers);
    Q_INVOKABLE void mouseWheel(bool up, int row, int col, int modifiers);

    // Resize the terminal (cols x rows), driven by the QML view.
    Q_INVOKABLE void resize(int cols, int rows);

    // Scroll the view up/down by `delta` lines (positive = up).
    Q_INVOKABLE void scrollBy(int delta);
    // Set the absolute scroll offset (lines up from the bottom; clamped).
    Q_INVOKABLE void scrollTo(int offset);
    // Jump to the bottom (newest content).
    Q_INVOKABLE void scrollToBottom();

    // Mouse selection, in visible-grid coordinates (row 0 = top of the view).
    Q_INVOKABLE void selPress(int row, int col);
    Q_INVOKABLE void selMove(int row, int col);
    Q_INVOKABLE void selRelease();
    Q_INVOKABLE void selClear();
    Q_INVOKABLE void selWord(int row, int col);
    Q_INVOKABLE void selectAll();

    // Per visible row, the selected column range (iTerm2-style): the start
    // line is clipped at the press column, middle lines span the full text,
    // the end line is clipped at the release column. -1 when the row is not
    // part of the selection.
    Q_INVOKABLE int selRowLeft(int visibleRow) const;
    Q_INVOKABLE int selRowRight(int visibleRow) const;

    // Clipboard.
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void paste();

    // One-frame snapshot of the visible grid for the QML renderer.
    Q_INVOKABLE QVariantList snapshot() const;
    Q_INVOKABLE int cursorRow() const;
    Q_INVOKABLE int cursorCol() const;
    Q_INVOKABLE bool cursorVisible() const;

    QString title() const { return m_title; }
    int rows() const { return m_rows; }
    int cols() const { return m_cols; }
    int scrollOffset() const { return m_terminal ? m_terminal->screen().scrollOffset() : 0; }
    int maxScrollOffset() const { return m_terminal ? m_terminal->screen().maxScrollOffset() : 0; }
    bool selectionActive() const { return m_selAnchor.line >= 0; }
    bool mouseTracking() const { return m_terminal && m_terminal->mouseTracking(); }
    bool mouseMotionTracking() const { return m_terminal && m_terminal->mouseMotionTracking(); }
    unsigned int backgroundColor() const { return m_defaultBg; }
    unsigned int foregroundColor() const { return m_defaultFg; }

signals:
    void frameReady();
    void titleChanged();
    void processExited(int code);
    void sizeChanged();
    void scrollOffsetChanged();
    void selectionChanged();
    void mouseTrackingChanged();
    void quitRequested();

private:
    void onData(const QByteArray &data);
    void onExited(int code);
    void onFrameTimer();
    void scheduleFrame();
    void checkScrollOffsetChanged();
    int visibleStart() const;
    // Last non-space column of an absolute line (full width if the line is blank).
    int lineTextEnd(int line) const;

    void editorShortcuts(int key);
    void sendMouseReport(int button, int row, int col, int event, int modifiers);

    static QString detectDefaultShell();
    static QRgb resolveColor(unsigned int encoded, int defaultColor);

    // Selection anchors, in absolute line+col (0 = oldest line). A line < 0
    // marks "no selection". Kept stable across output so a selection stays on
    // the same text while scrolling.
    struct AbsPos {
        int line{0};
        int col{0};
    };

    std::unique_ptr<PtyProcess> m_pty;
    std::unique_ptr<Terminal> m_terminal;

    QTimer m_frameTimer;
    QString m_title{QStringLiteral("Konsoll")};
    AbsPos m_selAnchor{.line = -1, .col = 0};
    AbsPos m_selCurrent{.line = -1, .col = 0};

    int m_rows{24};
    int m_cols{80};

    unsigned int m_defaultFg{0xffe0e0e0};
    unsigned int m_defaultBg{0xff141414};
};

} // namespace konsoll
