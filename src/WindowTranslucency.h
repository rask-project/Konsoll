#pragma once

#include <QObject>
#include <QtQml>

namespace konsoll {

class WindowTranslucency : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(uint color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal alpha READ alpha WRITE setAlpha NOTIFY alphaChanged)
    Q_PROPERTY(bool blurEnabled READ blurEnabled WRITE setBlurEnabled NOTIFY blurEnabledChanged)

public:
    explicit WindowTranslucency(QObject *parent = nullptr);

    QObject *window() const { return m_window; }
    void setWindow(QObject *window);

    uint color() const { return m_color; }
    void setColor(uint color);

    qreal alpha() const { return m_alpha; }
    void setAlpha(qreal alpha);

    bool blurEnabled() const { return m_blurEnabled; }
    void setBlurEnabled(bool enabled);

    Q_INVOKABLE void setWindowMovable(bool movable);

signals:
    void windowChanged();
    void colorChanged();
    void alphaChanged();
    void blurEnabledChanged();

private:
    void apply() const;

    QObject *m_window = nullptr;
    qreal m_alpha = 0.85;
    uint m_color = 0xff141414;
    bool m_blurEnabled = true;
};

} // namespace konsoll
