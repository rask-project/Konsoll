#pragma once

#include "TerminalBackend.h"
#include "term/Cell.h"

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QQuickItem>
#include <QRawFont>
#include <QTimer>
#include <QtQml>

#include <span>
#include <unordered_map>
#include <vector>

class QSGGeometryNode;
class QSGTextNode;

namespace konsoll
{

// Native scene-graph renderer for a TerminalBackend session.
//
// The visible grid is pulled from the session on the GUI thread (refresh()),
// diffed per row, and turned into scene-graph nodes in updatePaintNode()
// (render thread, GUI blocked):
//   root
//    ├─ background  QSGGeometryNode (vertex colors): cell backgrounds,
//    │              search/selection/command highlight, cursor block
//    ├─ text rows   one QSGTextNode per visible row, rebuilt only when that
//    │              row's cells changed (GPU glyph atlas, native rendering)
//    └─ overlay     QSGGeometryNode: underline, strike-through, link hover
class TerminalRenderer : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject *session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    // Cell metrics measured from `font` (advance of 'M', ascent + descent).
    // Read-only: the QML view sizes the grid and maps the mouse with them.
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY cellSizeChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY cellSizeChanged)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor selectedTextColor READ selectedTextColor WRITE setSelectedTextColor NOTIFY colorsChanged)
    Q_PROPERTY(int hoverRow READ hoverRow WRITE setHoverRow NOTIFY hoverChanged)
    Q_PROPERTY(int hoverStart READ hoverStart WRITE setHoverStart NOTIFY hoverChanged)
    Q_PROPERTY(int hoverEnd READ hoverEnd WRITE setHoverEnd NOTIFY hoverChanged)

public:
    explicit TerminalRenderer(QQuickItem *parent = nullptr);

    QObject *session() const { return m_session; }
    void setSession(QObject *session);
    QFont font() const { return m_baseFont; }
    void setFont(const QFont &font);
    qreal cellWidth() const { return m_cellWidth; }
    qreal cellHeight() const { return m_cellHeight; }
    QColor accentColor() const { return m_accent; }
    void setAccentColor(const QColor &color);
    QColor selectedTextColor() const { return m_selectedText; }
    void setSelectedTextColor(const QColor &color);
    int hoverRow() const { return m_hoverRow; }
    void setHoverRow(int row);
    int hoverStart() const { return m_hoverStart; }
    void setHoverStart(int col);
    int hoverEnd() const { return m_hoverEnd; }
    void setHoverEnd(int col);

signals:
    void sessionChanged();
    void fontChanged();
    void cellSizeChanged();
    void colorsChanged();
    void hoverChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    static constexpr int CursorBlinking = 0x01;
    static constexpr int CursorFocused = 0x02;
    static constexpr int CursorShapeShift = 4;

    struct Row {
        std::vector<Cell> cells; // resolved colors, selection/cursor applied
        int selLeft{-1};
        int selRight{-1};
        int searchLeft{-1};
        int searchRight{-1};
    };

    // Axis-aligned colored rectangle in item coordinates (non-premultiplied).
    struct ColorRect {
        float x;
        float y;
        float w;
        float h;
        QRgb color;
    };

    // Re-read the session's visible grid and schedule a repaint. Driven only
    // by TerminalBackend::frameReady (see setSession).
    void refresh();
    void rebuildFonts();
    void markAllDirty();
    void pullRow(int row, std::vector<Cell> &scratch);
    void appendBackgroundRects(std::vector<ColorRect> &rects) const;
    void appendRowBackground(std::vector<ColorRect> &rects, int row) const;
    void appendHighlightFrame(std::vector<ColorRect> &rects) const;
    void appendOverlayRects(std::vector<ColorRect> &rects) const;
    void syncTextRows(QSGNode &container);
    void buildRowText(QSGTextNode &node, int row);
    void addTextRun(QSGTextNode &node, std::span<const Cell> run, qreal x, qreal top) const;
    bool primaryFontCovers(const Cell &cell);
    const QFont &fontFor(const Cell &cell) const;
    void toggleBlink();
    void appendUnderline(std::vector<ColorRect> &rects, const Cell &cell, const QRectF &span) const;
    void appendCursor(std::vector<ColorRect> &rects, bool overlay) const;
    // Whether the cursor cell's glyph is drawn inverted (a filled block).
    bool cursorInvertsGlyph() const;
    int cursorShape() const { return m_cursorState >> CursorShapeShift; }
    void updateBlinkTimer();
    QRectF cellSpan(int row, int col, int count) const;

    static void uploadRects(QSGGeometryNode &node, const std::vector<ColorRect> &rects);

    // Members ordered heaviest-first for -Wpadded (see AGENTS.md).
    std::vector<Row> m_rows;
    std::vector<unsigned char> m_dirty; // per row: text node needs rebuild
    QPointer<TerminalBackend> m_session;
    QFont m_baseFont;
    QFont m_font;
    QFont m_boldFont;
    QFont m_italicFont;
    QFont m_boldItalicFont;
    std::unordered_map<char32_t, bool> m_coverage; // code point -> in m_font
    QRawFont m_rawFont; // primary family only, for coverage checks
    QTimer m_blinkTimer; // runs only while blinking cells or a blinking cursor are shown
    qreal m_cellWidth{8.0};
    qreal m_cellHeight{16.0};
    QColor m_accent{0x0a, 0x84, 0xff};
    QColor m_selectedText{Qt::white};
    float m_textOffsetY{0.0F};
    int m_cursorRow{-1}; // -1 when the cursor is hidden or scrolled away
    int m_cursorCol{-1};
    int m_highlightFirst{-1};
    int m_highlightLast{-1};
    int m_hoverRow{-1};
    int m_hoverStart{-1};
    int m_hoverEnd{-1};
    QRgb m_defaultBg{0xff141414};
    QRgb m_defaultFg{0xffe0e0e0};
    QRgb m_cursorColor{0xffe0e0e0};
    int m_blinkRows{0};  // rows holding SGR 5/6 blinking cells
    int m_blinkPhase{1}; // 1 = blinking text / cursor shown, 0 = hidden
    // CursorBlinking | CursorFocused, and the DECSCUSR shape (0 block,
    // 1 underline, 2 bar) from bit CursorShapeShift on.
    int m_cursorState{0};
};

} // namespace konsoll
