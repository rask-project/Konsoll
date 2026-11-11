#pragma once

#include <QObject>
#include <QtQml>

namespace konsoll
{

class WindowTranslucency : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(uint color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal alpha READ alpha WRITE setAlpha NOTIFY alphaChanged)
    Q_PROPERTY(bool blurEnabled READ blurEnabled WRITE setBlurEnabled NOTIFY blurEnabledChanged)
    Q_PROPERTY(bool fullScreen READ fullScreen WRITE setFullScreen NOTIFY fullScreenChanged)

public:
    explicit WindowTranslucency(QObject *parent = nullptr);

    QObject *window() const { return m_window; }
    void setWindow(QObject *window);

    uint color() const { return m_color; }
    void setColor(uint color);

    qreal alpha() const { return m_alpha; }
    void setAlpha(qreal alpha);

    bool blurEnabled() const { return (m_flags & BlurEnabled) != 0; }
    void setBlurEnabled(bool enabled);

    bool fullScreen() const { return (m_flags & FullScreen) != 0; }
    void setFullScreen(bool fullScreen);

    Q_INVOKABLE void setWindowMovable(bool movable);
    // A double-click on empty title-bar space: the system's action (zoom,
    // minimize or nothing).
    Q_INVOKABLE void titleBarDoubleClicked();

signals:
    void windowChanged();
    void colorChanged();
    void alphaChanged();
    void blurEnabledChanged();
    void fullScreenChanged();

private:
    static constexpr unsigned int BlurEnabled = 1u << 0;
    static constexpr unsigned int FullScreen = 1u << 1;

    void apply() const;
    void setFlag(unsigned int flag, bool on) { m_flags = on ? (m_flags | flag) : (m_flags & ~flag); }

    QObject *m_window = nullptr;
    qreal m_alpha = 0.85;
    uint m_color = 0xff141414;
    unsigned int m_flags = BlurEnabled; // BlurEnabled | FullScreen
};

} // namespace konsoll
