#include "TerminalRenderer.h"

#include <QFontMetricsF>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGTextNode>
#include <QSGVertexColorMaterial>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <ranges>
#include <span>
#include <string>

namespace konsoll
{

namespace
{

constexpr int kVerticesPerRect = 6;
constexpr float kSearchAlpha = 0.55F;
constexpr float kSelectionAlpha = 0.82F;
constexpr float kHighlightFillAlpha = 0.0175F;
constexpr float kHighlightStrokeAlpha = 0.65F;
constexpr unsigned int kDecorationFlags = CellFlag::Underline | CellFlag::Strike;
constexpr unsigned int kDecorationAttrs = CellAttr::Overline | CellAttr::UnderlineMask;
constexpr int kBlinkIntervalMs = 600;

QRgb withAlpha(const QColor &color, float alpha)
{
    return qRgba(color.red(), color.green(), color.blue(), static_cast<int>(std::lround(alpha * 255.0F)));
}

bool isBold(const Cell &cell)
{
    return (cell.flags & CellFlag::Bold) != 0u;
}

bool isInvisible(const Cell &cell)
{
    return (cell.flags & CellFlag::Invisible) != 0u;
}

bool isItalic(const Cell &cell)
{
    return (cell.attrs & CellAttr::Italic) != 0u;
}

bool isBlinking(const Cell &cell)
{
    return (cell.attrs & CellAttr::Blink) != 0u;
}

// Cells that can share one text layout: same color, face, visibility and
// blinking.
bool sameTextStyle(const Cell &a, const Cell &b)
{
    return a.fg == b.fg && isBold(a) == isBold(b) && isItalic(a) == isItalic(b) && isInvisible(a) == isInvisible(b) &&
           isBlinking(a) == isBlinking(b);
}

bool sameBackground(const Cell &a, const Cell &b)
{
    return a.bg == b.bg;
}

bool sameDecoration(const Cell &a, const Cell &b)
{
    return a.fg == b.fg && a.ul == b.ul && (a.flags & kDecorationFlags) == (b.flags & kDecorationFlags) &&
           (a.attrs & kDecorationAttrs) == (b.attrs & kDecorationAttrs);
}

bool isWidePart(const Cell &cell)
{
    return (cell.flags & (CellFlag::Wide | CellFlag::WideSpacer)) != 0u;
}

bool isBlank(const Cell &cell)
{
    return cell.ch == 0x20u || cell.ch == 0u;
}

std::unique_ptr<QSGGeometryNode> makeColorNode()
{
    auto node = std::make_unique<QSGGeometryNode>();
    auto geometry = std::make_unique<QSGGeometry>(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry.release());
    node->setMaterial(std::make_unique<QSGVertexColorMaterial>().release());
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

} // namespace

TerminalRenderer::TerminalRenderer(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_blinkTimer.setInterval(kBlinkIntervalMs);
    connect(&m_blinkTimer, &QTimer::timeout, this, &TerminalRenderer::toggleBlink);
    rebuildFonts();
}

void TerminalRenderer::setSession(QObject *session)
{
    auto *backend = qobject_cast<TerminalBackend *>(session);
    if (backend == m_session) {
        return;
    }
    if (m_session) {
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_session = backend;
    if (m_session) {
        // Only frameReady: it is trailing-edge coalesced and held back during
        // DEC 2026 synchronized updates. Other session signals (notably
        // scrollOffsetChanged, emitted on every PTY chunk) fire mid-redraw
        // and would paint torn frames — half-erased rows flashing as blocks.
        connect(m_session, &TerminalBackend::frameReady, this, &TerminalRenderer::refresh);
    }
    markAllDirty();
    refresh();
    emit sessionChanged();
}

void TerminalRenderer::setFont(const QFont &font)
{
    if (font == m_baseFont) {
        return;
    }
    m_baseFont = font;
    rebuildFonts();
    emit fontChanged();
}

void TerminalRenderer::setAccentColor(const QColor &color)
{
    if (color == m_accent) {
        return;
    }
    m_accent = color;
    update();
    emit colorsChanged();
}

void TerminalRenderer::setSelectedTextColor(const QColor &color)
{
    if (color == m_selectedText) {
        return;
    }
    m_selectedText = color;
    refresh();
    emit colorsChanged();
}

void TerminalRenderer::setHoverRow(int row)
{
    if (row == m_hoverRow) {
        return;
    }
    m_hoverRow = row;
    update();
    emit hoverChanged();
}

void TerminalRenderer::setHoverStart(int col)
{
    if (col == m_hoverStart) {
        return;
    }
    m_hoverStart = col;
    update();
    emit hoverChanged();
}

void TerminalRenderer::setHoverEnd(int col)
{
    if (col == m_hoverEnd) {
        return;
    }
    m_hoverEnd = col;
    update();
    emit hoverChanged();
}

void TerminalRenderer::rebuildFonts()
{
    // The QML font is used as-is, point size included: Qt Quick renders it
    // exactly like a QML Text with the same font.
    m_font = m_baseFont;
    m_font.setFamilies({m_baseFont.family(), QStringLiteral("JetBrainsMono Nerd Font Mono")});
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    m_font.setKerning(false);
    // One glyph per cell: ligatures and contextual alternates would merge or
    // shift glyphs across cell boundaries.
    const std::array<QFont::Tag, 5> shapingFeatures{
        QFont::Tag("liga"), QFont::Tag("clig"), QFont::Tag("dlig"), QFont::Tag("hlig"), QFont::Tag("calt")};
    std::ranges::for_each(shapingFeatures, [this](QFont::Tag tag) { m_font.setFeature(tag, 0); });

    m_boldFont = m_font;
    m_boldFont.setWeight(std::max(QFont::Bold, m_font.weight()));

    // The cell is the font's own advance and line box, so any monospace font
    // keeps its natural glyph size and spacing.
    const QFontMetricsF metrics(m_font);
    const qreal ascentDescent = metrics.ascent() + metrics.descent();
    const qreal cellWidth = std::max(1.0, metrics.horizontalAdvance(QLatin1Char('M')));
    const qreal cellHeight = std::max(1.0, std::ceil(ascentDescent));

    // A bold face can be a touch wider than the regular one; pull it back
    // onto the grid so bold runs don't drift.
    const qreal boldAdvance = QFontMetricsF(m_boldFont).horizontalAdvance(QLatin1Char('M'));
    m_boldFont.setLetterSpacing(QFont::AbsoluteSpacing, cellWidth - boldAdvance);

    // Italic faces (or synthesized obliques) get the same grid correction.
    m_italicFont = m_font;
    m_italicFont.setItalic(true);
    m_italicFont.setLetterSpacing(QFont::AbsoluteSpacing,
                                  cellWidth - QFontMetricsF(m_italicFont).horizontalAdvance(QLatin1Char('M')));
    m_boldItalicFont = m_boldFont;
    m_boldItalicFont.setItalic(true);
    m_boldItalicFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    m_boldItalicFont.setLetterSpacing(QFont::AbsoluteSpacing,
                                      cellWidth - QFontMetricsF(m_boldItalicFont).horizontalAdvance(QLatin1Char('M')));

    m_textOffsetY = static_cast<float>(std::floor((cellHeight - ascentDescent) / 2.0));
    m_rawFont = QRawFont::fromFont(m_font);
    m_coverage.clear();

    const bool cellSizeChanged = !qFuzzyCompare(cellWidth, m_cellWidth) || !qFuzzyCompare(cellHeight, m_cellHeight);
    m_cellWidth = cellWidth;
    m_cellHeight = cellHeight;

    markAllDirty();
    update();
    if (cellSizeChanged) {
        emit this->cellSizeChanged();
    }
}

void TerminalRenderer::markAllDirty()
{
    std::ranges::fill(m_dirty, static_cast<unsigned char>(1));
}

void TerminalRenderer::refresh()
{
    if (!m_session) {
        m_rows.clear();
        m_dirty.clear();
        update();
        return;
    }

    const TerminalBackend &s = *m_session;
    m_defaultBg = static_cast<QRgb>(s.backgroundColor());
    m_defaultFg = static_cast<QRgb>(s.foregroundColor());
    m_cursorColor = static_cast<QRgb>(s.cursorColor());
    const bool cursorShown = s.cursorVisible() && s.scrollOffset() == 0;
    const int cursorRow = cursorShown ? s.cursorRow() : -1;
    if (cursorRow != m_cursorRow || s.cursorCol() != m_cursorCol) {
        // A moving cursor stays visible: restart the blink cycle.
        m_blinkPhase = 1;
        if (m_blinkTimer.isActive()) {
            m_blinkTimer.start();
        }
    }
    m_cursorRow = cursorRow;
    m_cursorCol = s.cursorCol();
    m_cursorState = (s.cursorBlinking() ? CursorBlinking : 0) | (s.focused() ? CursorFocused : 0) |
                    (s.cursorShape() << CursorShapeShift);
    m_highlightFirst = s.highlightActive() ? s.highlightStartRow() : -1;
    m_highlightLast = s.highlightActive() ? s.highlightEndRow() : -1;

    const auto rows = static_cast<std::size_t>(s.gridRows());
    if (m_rows.size() != rows) {
        m_rows.resize(rows);
        m_dirty.assign(rows, 1);
    }

    std::vector<Cell> scratch;
    std::ranges::for_each(std::views::iota(0, s.gridRows()), [&](int row) { pullRow(row, scratch); });
    m_images = s.visibleImages();

    m_blinkRows = static_cast<int>(
        std::ranges::count_if(m_rows, [](const Row &r) { return std::ranges::any_of(r.cells, isBlinking); }));
    updateBlinkTimer();
    update();
}

void TerminalRenderer::updateBlinkTimer()
{
    // A blinking cursor only blinks while focused, as in native terminals.
    const bool cursorBlinks = m_cursorRow >= 0 && (m_cursorState & CursorBlinking) != 0 &&
                              (m_cursorState & CursorFocused) != 0;
    const bool needed = m_blinkRows > 0 || cursorBlinks;
    if (needed && !m_blinkTimer.isActive()) {
        m_blinkTimer.start();
    } else if (!needed && m_blinkTimer.isActive()) {
        m_blinkTimer.stop();
        m_blinkPhase = 1;
    }
}

bool TerminalRenderer::cursorInvertsGlyph() const
{
    const bool blinkHidden = (m_cursorState & CursorBlinking) != 0 && m_blinkPhase == 0;
    return cursorShape() == 0 && (m_cursorState & CursorFocused) != 0 && !blinkHidden;
}

void TerminalRenderer::toggleBlink()
{
    m_blinkPhase = 1 - m_blinkPhase;
    // Only rows holding blinking cells need their text rebuilt; the cursor
    // row is re-pulled so its glyph color follows the cursor block.
    std::ranges::for_each(std::views::iota(std::size_t{0}, m_rows.size()), [this](std::size_t row) {
        if (std::ranges::any_of(m_rows[row].cells, isBlinking)) {
            m_dirty[row] = 1;
        }
    });
    if (m_session && m_cursorRow >= 0 && m_cursorRow < static_cast<int>(m_rows.size())) {
        std::vector<Cell> scratch;
        pullRow(m_cursorRow, scratch);
    }
    update();
}

void TerminalRenderer::pullRow(int row, std::vector<Cell> &scratch)
{
    const TerminalBackend &s = *m_session;
    Row &target = m_rows[static_cast<std::size_t>(row)];
    s.visibleRowCells(row, scratch);

    target.selLeft = s.selectionActive() ? s.selRowLeft(row) : -1;
    target.selRight = s.selectionActive() ? s.selRowRight(row) : -1;
    target.searchLeft = s.searchRowLeft(row);
    target.searchRight = s.searchRowRight(row);

    // Bake selection and cursor glyph colors into the cells so the per-row
    // diff below also catches selection/cursor moves.
    const int cols = static_cast<int>(scratch.size());
    if (target.selLeft >= 0 && target.selRight >= target.selLeft && target.selLeft < cols) {
        const QRgb selectedFg = m_selectedText.rgb();
        const auto first = scratch.begin() + target.selLeft;
        const auto last = scratch.begin() + std::min(target.selRight + 1, cols);
        std::ranges::for_each(first, last, [selectedFg](Cell &cell) { cell.fg = selectedFg; });
    }
    if (row == m_cursorRow && m_cursorCol >= 0 && m_cursorCol < cols && cursorInvertsGlyph()) {
        scratch[static_cast<std::size_t>(m_cursorCol)].fg = m_defaultBg;
    }

    if (scratch != target.cells) {
        target.cells.swap(scratch);
        m_dirty[static_cast<std::size_t>(row)] = 1;
    }
}

QRectF TerminalRenderer::cellSpan(int row, int col, int count) const
{
    return {col * m_cellWidth, row * m_cellHeight, count * m_cellWidth, m_cellHeight};
}

QSGNode *TerminalRenderer::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData * /*data*/)
{
    std::unique_ptr<QSGNode> created;
    QSGNode *root = oldNode;
    if (root == nullptr) {
        created = std::make_unique<QSGNode>();
        created->appendChildNode(makeColorNode().release());                  // background
        created->appendChildNode(std::make_unique<QSGNode>().release());        // images below the text
        created->appendChildNode(std::make_unique<QSGNode>().release());        // text rows
        created->appendChildNode(std::make_unique<QSGNode>().release());        // images above the text
        created->appendChildNode(makeColorNode().release());                  // overlay
        created->appendChildNode(std::make_unique<ImageCacheNode>().release()); // image textures
        root = created.get();
        markAllDirty();
    }

    auto &background = static_cast<QSGGeometryNode &>(*root->childAtIndex(0));
    QSGNode &imagesBelow = *root->childAtIndex(1);
    QSGNode &textRows = *root->childAtIndex(2);
    QSGNode &imagesAbove = *root->childAtIndex(3);
    auto &overlay = static_cast<QSGGeometryNode &>(*root->childAtIndex(4));
    auto &imageCache = static_cast<ImageCacheNode &>(*root->childAtIndex(5));

    std::vector<ColorRect> rects;
    appendBackgroundRects(rects);
    uploadRects(background, rects);

    syncTextRows(textRows);
    syncImages(imagesBelow, imagesAbove, imageCache);

    rects.clear();
    appendOverlayRects(rects);
    uploadRects(overlay, rects);

    if (created) {
        return created.release();
    }
    return root;
}

void TerminalRenderer::appendBackgroundRects(std::vector<ColorRect> &rects) const
{
    std::ranges::for_each(std::views::iota(0, static_cast<int>(m_rows.size())),
                          [&](int row) { appendRowBackground(rects, row); });
    appendHighlightFrame(rects);

    appendCursor(rects, false);
}

// The filled block goes under the text (its glyph is drawn inverted); the
// underline, bar and the hollow unfocused block go over it.
void TerminalRenderer::appendCursor(std::vector<ColorRect> &rects, bool overlay) const
{
    const bool cursorInGrid = m_cursorRow >= 0 && m_cursorRow < static_cast<int>(m_rows.size()) && m_cursorCol >= 0 &&
                              m_cursorCol < static_cast<int>(m_rows[static_cast<std::size_t>(m_cursorRow)].cells.size());
    const bool blinkHidden = (m_cursorState & CursorBlinking) != 0 && m_blinkPhase == 0 &&
                             (m_cursorState & CursorFocused) != 0;
    if (!cursorInGrid || blinkHidden || overlay == cursorInvertsGlyph()) {
        return;
    }
    const std::vector<Cell> &cells = m_rows[static_cast<std::size_t>(m_cursorRow)].cells;
    const bool onWide = (cells[static_cast<std::size_t>(m_cursorCol)].flags & CellFlag::Wide) != 0u &&
                        m_cursorCol + 1 < static_cast<int>(cells.size());
    const QRectF r = cellSpan(m_cursorRow, m_cursorCol, onWide ? 2 : 1);
    const auto x = static_cast<float>(r.x());
    const auto y = static_cast<float>(r.y());
    const auto w = static_cast<float>(r.width());
    const auto h = static_cast<float>(r.height());
    constexpr float kThickness = 2.0F;
    if (!overlay) {
        rects.push_back({.x = x, .y = y, .w = w, .h = h, .color = m_cursorColor});
    } else if (cursorShape() == 1) {
        rects.push_back({.x = x, .y = y + h - kThickness, .w = w, .h = kThickness, .color = m_cursorColor});
    } else if (cursorShape() == 2) {
        rects.push_back({.x = x, .y = y, .w = kThickness, .h = h, .color = m_cursorColor});
    } else {
        // Unfocused block: a 1 px outline.
        rects.push_back({.x = x, .y = y, .w = w, .h = 1.0F, .color = m_cursorColor});
        rects.push_back({.x = x, .y = y + h - 1.0F, .w = w, .h = 1.0F, .color = m_cursorColor});
        rects.push_back({.x = x, .y = y + 1.0F, .w = 1.0F, .h = h - 2.0F, .color = m_cursorColor});
        rects.push_back({.x = x + w - 1.0F, .y = y + 1.0F, .w = 1.0F, .h = h - 2.0F, .color = m_cursorColor});
    }
}

void TerminalRenderer::appendRowBackground(std::vector<ColorRect> &rects, int row) const
{
    const Row &data = m_rows[static_cast<std::size_t>(row)];
    const auto push = [&](int col, int count, QRgb color) {
        const QRectF r = cellSpan(row, col, count);
        rects.push_back({.x = static_cast<float>(r.x()),
                         .y = static_cast<float>(r.y()),
                         .w = static_cast<float>(r.width()),
                         .h = static_cast<float>(r.height()),
                         .color = color});
    };

    // Default-background cells are left empty so the window translucency
    // shows through.
    int col = 0;
    for (const auto run : data.cells | std::views::chunk_by(sameBackground)) {
        const int count = static_cast<int>(std::ranges::size(run));
        if (run.front().bg != m_defaultBg) {
            push(col, count, run.front().bg);
        }
        col += count;
    }
    if (data.searchLeft >= 0 && data.searchRight >= data.searchLeft) {
        push(data.searchLeft, data.searchRight - data.searchLeft + 1, withAlpha(m_accent, kSearchAlpha));
    }
    if (data.selLeft >= 0 && data.selRight >= data.selLeft) {
        push(data.selLeft, data.selRight - data.selLeft + 1, withAlpha(m_accent, kSelectionAlpha));
    }
}

void TerminalRenderer::appendHighlightFrame(std::vector<ColorRect> &rects) const
{
    if (m_highlightFirst < 0 || m_highlightLast < m_highlightFirst || m_rows.empty()) {
        return;
    }
    const int cols = static_cast<int>(m_rows.front().cells.size());
    const QRectF box = cellSpan(m_highlightFirst, 0, cols).united(cellSpan(m_highlightLast, 0, cols));
    const auto x = static_cast<float>(box.x());
    const auto y = static_cast<float>(box.y());
    const auto w = static_cast<float>(box.width());
    const auto h = static_cast<float>(box.height());
    const QRgb stroke = withAlpha(m_accent, kHighlightStrokeAlpha);

    rects.push_back({.x = x, .y = y, .w = w, .h = h, .color = withAlpha(m_accent, kHighlightFillAlpha)});
    rects.push_back({.x = x, .y = y, .w = w, .h = 1.0F, .color = stroke});
    rects.push_back({.x = x, .y = y + h - 1.0F, .w = w, .h = 1.0F, .color = stroke});
    rects.push_back({.x = x, .y = y + 1.0F, .w = 1.0F, .h = h - 2.0F, .color = stroke});
    rects.push_back({.x = x + w - 1.0F, .y = y + 1.0F, .w = 1.0F, .h = h - 2.0F, .color = stroke});
}

void TerminalRenderer::appendOverlayRects(std::vector<ColorRect> &rects) const
{
    const auto cellHeight = static_cast<float>(m_cellHeight);
    const auto strikeY = std::floor(cellHeight / 2.0F);

    std::ranges::for_each(std::views::iota(0, static_cast<int>(m_rows.size())), [&](int row) {
        int col = 0;
        for (const auto run : m_rows[static_cast<std::size_t>(row)].cells | std::views::chunk_by(sameDecoration)) {
            const int count = static_cast<int>(std::ranges::size(run));
            const Cell &cell = run.front();
            const QRectF r = cellSpan(row, col, count);
            const auto x = static_cast<float>(r.x());
            const auto y = static_cast<float>(r.y());
            const auto w = static_cast<float>(r.width());
            if ((cell.flags & CellFlag::Underline) != 0u) {
                appendUnderline(rects, cell, r);
            }
            if ((cell.flags & CellFlag::Strike) != 0u) {
                rects.push_back({.x = x, .y = y + strikeY, .w = w, .h = 1.0F, .color = cell.fg});
            }
            if ((cell.attrs & CellAttr::Overline) != 0u) {
                rects.push_back({.x = x, .y = y, .w = w, .h = 1.0F, .color = cell.fg});
            }
            col += count;
        }
    });

    appendCursor(rects, true);

    if (m_hoverRow >= 0 && m_hoverStart >= 0 && m_hoverEnd > m_hoverStart) {
        const QRectF r = cellSpan(m_hoverRow, m_hoverStart, m_hoverEnd - m_hoverStart);
        rects.push_back({.x = static_cast<float>(r.x()),
                         .y = static_cast<float>(r.y()) + cellHeight - 2.0F,
                         .w = static_cast<float>(r.width()),
                         .h = 1.0F,
                         .color = withAlpha(m_accent, 1.0F)});
    }
}

// SGR 4:n underline in the SGR 58 color (resolved into `cell.ul`), drawn
// along the bottom of the cell span: single, double, curly (a 2 px stepped
// wave), dotted or dashed.
void TerminalRenderer::appendUnderline(std::vector<ColorRect> &rects, const Cell &cell, const QRectF &span) const
{
    const auto x = static_cast<float>(span.x());
    const auto w = static_cast<float>(span.width());
    const auto bottom = static_cast<float>(span.y() + m_cellHeight) - 1.0F;
    const QRgb color = cell.ul;
    const auto segments = [&](float step, float length, const auto &yAt) {
        const int count = static_cast<int>(std::ceil(w / step));
        std::ranges::for_each(std::views::iota(0, count), [&](int i) {
            const float sx = x + (static_cast<float>(i) * step);
            rects.push_back({.x = sx, .y = yAt(i), .w = std::min(length, x + w - sx), .h = 1.0F, .color = color});
        });
    };

    switch (underlineStyle(cell)) {
    case UnderlineStyle::Double:
        rects.push_back({.x = x, .y = bottom, .w = w, .h = 1.0F, .color = color});
        rects.push_back({.x = x, .y = bottom - 2.0F, .w = w, .h = 1.0F, .color = color});
        break;
    case UnderlineStyle::Curly: {
        constexpr std::array<float, 4> kWave{0.0F, 1.0F, 2.0F, 1.0F};
        segments(2.0F, 2.0F, [&](int i) { return bottom - kWave[static_cast<std::size_t>(i) % kWave.size()]; });
        break;
    }
    case UnderlineStyle::Dotted:
        segments(2.0F, 1.0F, [bottom](int) { return bottom; });
        break;
    case UnderlineStyle::Dashed:
        segments(6.0F, 4.0F, [bottom](int) { return bottom; });
        break;
    default:
        rects.push_back({.x = x, .y = bottom, .w = w, .h = 1.0F, .color = color});
        break;
    }
}

// Kitty / sixel images: rebuilt each frame from the cached textures; a
// texture lives as long as its image is on screen.
void TerminalRenderer::syncImages(QSGNode &below, QSGNode &above, ImageCacheNode &cache)
{
    const auto clearLayer = [](QSGNode &layer) {
        while (layer.childCount() > 0) {
            QSGNode *const child = layer.firstChild();
            layer.removeChildNode(child);
            const std::unique_ptr<QSGNode> discard(child);
        }
    };
    clearLayer(below);
    clearLayer(above);

    std::unordered_map<std::uint64_t, std::unique_ptr<QSGTexture>> used;
    std::ranges::for_each(m_images, [&](const TerminalBackend::VisibleImage &visible) {
        auto cached = cache.textures.find(visible.serial);
        if (cached != cache.textures.end()) {
            used.emplace(visible.serial, std::move(cached->second));
            cache.textures.erase(cached);
        } else if (used.find(visible.serial) == used.end()) {
            used.emplace(visible.serial, std::unique_ptr<QSGTexture>(window()->createTextureFromImage(*visible.image)));
        }
        std::unique_ptr<QSGImageNode> node(window()->createImageNode());
        node->setTexture(used.at(visible.serial).get());
        node->setOwnsTexture(false);
        node->setFiltering(QSGTexture::Linear);
        node->setSourceRect(QRectF(visible.source));
        node->setRect(QRectF(visible.target.x() * m_cellWidth, visible.target.y() * m_cellHeight,
                             visible.target.width() * m_cellWidth, visible.target.height() * m_cellHeight));
        (visible.z < 0 ? below : above).appendChildNode(node.release());
    });
    // Textures of images no longer shown are freed here (render thread).
    cache.textures = std::move(used);
}

void TerminalRenderer::uploadRects(QSGGeometryNode &node, const std::vector<ColorRect> &rects)
{
    QSGGeometry *geometry = node.geometry();
    geometry->allocate(static_cast<int>(rects.size()) * kVerticesPerRect);
    const std::span vertices(geometry->vertexDataAsColoredPoint2D(), static_cast<std::size_t>(geometry->vertexCount()));

    std::size_t index = 0;
    std::ranges::for_each(rects, [&](const ColorRect &rect) {
        // QSGVertexColorMaterial expects premultiplied colors.
        const int alpha = qAlpha(rect.color);
        const auto channel = [alpha](int value) { return static_cast<uchar>((value * alpha) / 255); };
        const uchar r = channel(qRed(rect.color));
        const uchar g = channel(qGreen(rect.color));
        const uchar b = channel(qBlue(rect.color));
        const auto a = static_cast<uchar>(alpha);
        const float x2 = rect.x + rect.w;
        const float y2 = rect.y + rect.h;
        vertices[index + 0].set(rect.x, rect.y, r, g, b, a);
        vertices[index + 1].set(x2, rect.y, r, g, b, a);
        vertices[index + 2].set(rect.x, y2, r, g, b, a);
        vertices[index + 3].set(x2, rect.y, r, g, b, a);
        vertices[index + 4].set(x2, y2, r, g, b, a);
        vertices[index + 5].set(rect.x, y2, r, g, b, a);
        index += kVerticesPerRect;
    });
    node.markDirty(QSGNode::DirtyGeometry);
}

void TerminalRenderer::syncTextRows(QSGNode &container)
{
    const int wanted = static_cast<int>(m_rows.size());
    while (container.childCount() > wanted) {
        QSGNode *last = container.lastChild();
        container.removeChildNode(last);
        const std::unique_ptr<QSGNode> discard(last);
    }
    while (container.childCount() < wanted) {
        std::unique_ptr<QSGTextNode> node(window()->createTextNode());
        node->setRenderType(QSGTextNode::NativeRendering);
        container.appendChildNode(node.release());
        m_dirty[static_cast<std::size_t>(container.childCount() - 1)] = 1;
    }

    // QSGNode children are an intrusive linked list; there is no range to
    // hand to an algorithm.
    std::size_t row = 0;
    for (QSGNode *child = container.firstChild(); child != nullptr; child = child->nextSibling()) {
        if (m_dirty[row] != 0) {
            buildRowText(static_cast<QSGTextNode &>(*child), static_cast<int>(row));
            m_dirty[row] = 0;
        }
        ++row;
    }
}

const QFont &TerminalRenderer::fontFor(const Cell &cell) const
{
    if (isItalic(cell)) {
        return isBold(cell) ? m_boldItalicFont : m_italicFont;
    }
    return isBold(cell) ? m_boldFont : m_font;
}

bool TerminalRenderer::primaryFontCovers(const Cell &cell)
{
    if (isWidePart(cell)) {
        return false; // wide glyphs are drawn on their own, spanning two cells
    }
    if (cell.ch < 0x80u) {
        return true; // ASCII: present in any monospace font
    }
    const auto [entry, inserted] = m_coverage.try_emplace(static_cast<char32_t>(cell.ch), false);
    if (inserted) {
        entry->second = m_rawFont.supportsCharacter(cell.ch);
    }
    return entry->second;
}

void TerminalRenderer::buildRowText(QSGTextNode &node, int row)
{
    node.clear();
    const qreal top = (row * m_cellHeight) + m_textOffsetY;

    // A glyph missing from the primary font is drawn by a fallback font with
    // a different advance, which would push the rest of a shared layout off
    // the grid. Such cells never join a run: each gets its own layout pinned
    // to its column.
    const auto sameRun = [this](const Cell &a, const Cell &b) {
        return sameTextStyle(a, b) && primaryFontCovers(a) && primaryFontCovers(b);
    };

    int col = 0;
    for (const auto run : m_rows[static_cast<std::size_t>(row)].cells | std::views::chunk_by(sameRun)) {
        addTextRun(node, std::span<const Cell>(run.begin(), run.end()), col * m_cellWidth, top);
        col += static_cast<int>(std::ranges::size(run));
    }
}

void TerminalRenderer::addTextRun(QSGTextNode &node, std::span<const Cell> run, qreal x, qreal top) const
{
    if (isInvisible(run.front()) || (isBlinking(run.front()) && m_blinkPhase == 0) || std::ranges::all_of(run, isBlank)) {
        return;
    }

    std::u32string codePoints;
    codePoints.reserve(run.size());
    std::ranges::transform(run, std::back_inserter(codePoints),
                           [](const Cell &cell) { return isBlank(cell) ? U' ' : static_cast<char32_t>(cell.ch); });

    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    QTextLayout layout(QString::fromUcs4(codePoints.data(), static_cast<qsizetype>(codePoints.size())),
                       fontFor(run.front()));
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setPosition({0.0, 0.0});
    layout.endLayout();

    node.setColor(QColor::fromRgb(run.front().fg));
    node.addTextLayout(QPointF(x, top), &layout);
}

} // namespace konsoll
