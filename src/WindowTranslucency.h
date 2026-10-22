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

public:
    explicit WindowTranslucency(QObject *parent = nullptr);

    QObject *window() const { return m_window; }
    void setWindow(QObject *window);

    uint color() const { return m_color; }
    void setColor(uint color);

    qreal alpha() const { return m_alpha; }
    void setAlpha(qreal alpha);

signals:
    void windowChanged();
    void colorChanged();
    void alphaChanged();

private:
    void apply() const;

    QObject *m_window = nullptr;
    uint m_color = 0xff141414;
    qreal m_alpha = 0.85;
};

} // namespace konsoll
