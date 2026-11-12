#pragma once

#include <QObject>
#include <QtQml>

namespace konsoll
{

// Settings > Terminal: how many history lines each session keeps
// (0 = unlimited). Saved as General/scrollbackLines; every session reads it
// when it starts and follows changes while it runs, background sessions
// included.
class ScrollbackSettings final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(ScrollbackSettings)
    QML_SINGLETON

    Q_PROPERTY(int maxLines READ maxLines WRITE setMaxLines NOTIFY maxLinesChanged)

public:
    // The one instance, shared by the sessions and QML.
    static ScrollbackSettings &shared();
    static ScrollbackSettings *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    int maxLines() const { return static_cast<int>(m_maxLines); }
    void setMaxLines(int lines);

signals:
    void maxLinesChanged();

private:
    ScrollbackSettings();

    long long m_maxLines = 0; // pointer-sized: no tail padding after QObject
};

} // namespace konsoll
