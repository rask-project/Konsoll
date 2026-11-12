#include "ScrollbackSettings.h"

#include <QSettings>

#include <algorithm>

namespace konsoll
{

namespace
{
const QString kKey = QStringLiteral("General/scrollbackLines");
} // namespace

ScrollbackSettings::ScrollbackSettings() : m_maxLines(std::max(0, QSettings().value(kKey, 0).toInt())) {}

ScrollbackSettings &ScrollbackSettings::shared()
{
    static ScrollbackSettings instance;
    return instance;
}

ScrollbackSettings *ScrollbackSettings::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    Q_UNUSED(qmlEngine);
    Q_UNUSED(jsEngine);
    ScrollbackSettings &settings = shared();
    // A function-local static: the engine must not delete it.
    QJSEngine::setObjectOwnership(&settings, QJSEngine::CppOwnership);
    return &settings;
}

void ScrollbackSettings::setMaxLines(int lines)
{
    lines = std::max(0, lines);
    if (lines == m_maxLines) {
        return;
    }
    m_maxLines = lines;
    QSettings().setValue(kKey, lines);
    emit maxLinesChanged();
}

} // namespace konsoll
