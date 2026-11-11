#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <cstdint>

namespace konsoll
{

// A QML item backed by a native Liquid Glass view (NSGlassEffectView, macOS
// 26+). The native view lives behind the Qt content view and follows this
// item's scene geometry, effective visibility and effective opacity every
// frame, so QML content drawn over the item appears on top of real glass.
// Where `supported` is false nothing native is created and QML should fall
// back to its own background.
class GlassEffect : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged)
    Q_PROPERTY(QColor tint READ tint WRITE setTint NOTIFY tintChanged)
    // Clear (more transparent) glass instead of the regular frosted style.
    Q_PROPERTY(bool clear READ clear WRITE setClear NOTIFY clearChanged)

public:
    explicit GlassEffect(QQuickItem *parent = nullptr);
    ~GlassEffect() override;

    GlassEffect(const GlassEffect &) = delete;
    GlassEffect &operator=(const GlassEffect &) = delete;
    GlassEffect(GlassEffect &&) = delete;
    GlassEffect &operator=(GlassEffect &&) = delete;

    [[nodiscard]] static bool supported();

    [[nodiscard]] qreal cornerRadius() const { return m_cornerRadius; }
    void setCornerRadius(qreal radius);

    [[nodiscard]] QColor tint() const { return QColor::fromRgba(m_tint); }
    void setTint(const QColor &tint);

    [[nodiscard]] bool clear() const { return m_clear; }
    void setClear(bool clear);

signals:
    void cornerRadiusChanged();
    void tintChanged();
    void clearChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void attach(QQuickWindow *window);
    void detach();
    void sync();
    void markDirty();
    [[nodiscard]] qreal effectiveOpacity() const;

    QRectF m_lastRect;
    QString m_identifier;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_frameConnection;
    WId m_winId = 0;
    qreal m_cornerRadius = 8.0;
    QRgb m_tint = 0U;
    // Effective opacity last pushed to the native view, quantized to 8 bits.
    std::uint8_t m_lastAlpha = 0U;
    bool m_clear = false;
    bool m_created = false;
    bool m_dirty = true;
};

} // namespace konsoll
