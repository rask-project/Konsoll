#pragma once

#include "pty/PtyProcess.h"
#include "term/Terminal.h"

#include <QColor>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml>

#include <memory>

namespace konsoll {

// The QML<->C++ bridge. Owns the PTY and the Terminal (screen model), feeds
// parsed output, coalesces repaints to ~60fps, and exposes a snapshot to QML.
class TerminalBackend : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Backend)
    QML_SINGLETON
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int rows READ rows NOTIFY sizeChanged)
    Q_PROPERTY(int cols READ cols NOTIFY sizeChanged)
    Q_PROPERTY(int scrollOffset READ scrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(int maxScrollOffset READ maxScrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(unsigned int backgroundColor READ backgroundColor CONSTANT)
    Q_PROPERTY(unsigned int foregroundColor READ foregroundColor CONSTANT)

public:
    explicit TerminalBackend(QObject *parent = nullptr);

    // Start the terminal with the detected default shell.
    Q_INVOKABLE void start();

    // Input from QML.
    Q_INVOKABLE void keyPress(int key, const QString &text, int modifiers);
    Q_INVOKABLE void writeText(const QString &text);

    // Resize the terminal (cols x rows), driven by the QML view.
    Q_INVOKABLE void resize(int cols, int rows);

    // Scroll the view up/down by `delta` lines (positive = up).
    Q_INVOKABLE void scrollBy(int delta);
    // Set the absolute scroll offset (lines up from the bottom; clamped).
    Q_INVOKABLE void scrollTo(int offset);
    // Jump to the bottom (newest content).
    Q_INVOKABLE void scrollToBottom();

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
    unsigned int backgroundColor() const { return m_defaultBg; }
    unsigned int foregroundColor() const { return m_defaultFg; }

signals:
    void frameReady();
    void titleChanged();
    void processExited(int code);
    void sizeChanged();
    void scrollOffsetChanged();
    void quitRequested();

private:
    void onData(const QByteArray &data);
    void onExited(int code);
    void onFrameTimer();
    void scheduleFrame();
    void checkScrollOffsetChanged();

    static QString detectDefaultShell();
    static QRgb resolveColor(unsigned int encoded, int defaultColor);

    std::unique_ptr<PtyProcess> m_pty;
    std::unique_ptr<Terminal> m_terminal;
    QTimer m_frameTimer;

    QString m_title = QStringLiteral("Konsoll");
    int m_rows = 24;
    int m_cols = 80;
    unsigned int m_defaultFg = 0xffe0e0e0;
    unsigned int m_defaultBg = 0xff141414;
};

} // namespace konsoll
