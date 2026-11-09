#include "GlassEffect.h"
#include "MacOSTranslucency.h"

#include <QUuid>

#include <algorithm>

namespace konsoll
{

GlassEffect::GlassEffect(QQuickItem *parent)
    : QQuickItem(parent)
    , m_identifier(QUuid::createUuid().toString(QUuid::WithoutBraces))
{}

GlassEffect::~GlassEffect()
{
    detach();
}

bool GlassEffect::supported()
{
    return isNativeGlassSupported();
}

void GlassEffect::setCornerRadius(qreal radius)
{
    if (qFuzzyCompare(m_cornerRadius, radius)) {
        return;
    }
    m_cornerRadius = radius;
    emit cornerRadiusChanged();
    markDirty();
}

void GlassEffect::setTint(const QColor &tint)
{
    if (m_tint == tint.rgba()) {
        return;
    }
    m_tint = tint.rgba();
    emit tintChanged();
    markDirty();
}

void GlassEffect::setClear(bool clear)
{
    if (m_clear == clear) {
        return;
    }
    m_clear = clear;
    emit clearChanged();
    markDirty();
}

void GlassEffect::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        detach();
        attach(value.window);
    } else if (change == ItemVisibleHasChanged || change == ItemOpacityHasChanged) {
        markDirty();
    }
}

void GlassEffect::attach(QQuickWindow *window)
{
    if (window == nullptr || !supported()) {
        return;
    }
    m_window = window;
    // Ancestors can move this item without notifying it (e.g. animated tab
    // positions), so re-check the scene geometry on every animation tick.
    m_frameConnection = connect(window, &QQuickWindow::afterAnimating, this, &GlassEffect::sync);
    markDirty();
}

void GlassEffect::detach()
{
    disconnect(m_frameConnection);
    if (m_created && !m_window.isNull() && m_window->handle() != nullptr) {
        destroyNativeGlassView(m_winId, m_identifier);
    }
    m_window.clear();
    m_created = false;
    m_winId = 0;
    m_lastRect = {};
    m_lastAlpha = 0U;
}

void GlassEffect::markDirty()
{
    m_dirty = true;
    if (!m_window.isNull()) {
        m_window->update();
    }
}

qreal GlassEffect::effectiveOpacity() const
{
    qreal opacity = 1.0;
    const QQuickItem *item = this;
    while (item != nullptr) {
        opacity *= item->opacity();
        item = item->parentItem();
    }
    return opacity;
}

void GlassEffect::sync()
{
    if (m_window.isNull() || m_window->handle() == nullptr) {
        return;
    }
    if (!m_created) {
        m_winId = m_window->winId();
        m_created = createNativeGlassView(m_winId, m_identifier);
        if (!m_created) {
            return;
        }
        m_dirty = true;
    }

    QRectF const rect = mapRectToScene(boundingRect());
    qreal const opacity = isVisible() ? std::clamp(effectiveOpacity(), 0.0, 1.0) : 0.0;
    auto const alpha = static_cast<std::uint8_t>(qRound(opacity * 255.0));
    if (!m_dirty && rect == m_lastRect && alpha == m_lastAlpha) {
        return;
    }

    m_dirty = false;
    m_lastRect = rect;
    m_lastAlpha = alpha;
    updateNativeGlassView(
        m_winId,
        m_identifier,
        rect,
        m_cornerRadius,
        QColor::fromRgba(m_tint),
        alpha / 255.0,
        m_clear
    );
}

} // namespace konsoll
