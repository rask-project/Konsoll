#include "LinkSettings.h"

#include <QSettings>

namespace konsoll
{

namespace
{
const QString kCommandKey = QStringLiteral("General/editorCommand");
const QString kInTerminalKey = QStringLiteral("General/editorInTerminal");
} // namespace

LinkSettings::LinkSettings()
    : m_editorCommand(QSettings().value(kCommandKey).toString().trimmed())
    , m_editorInTerminal(QSettings().value(kInTerminalKey, false).toBool() ? 1u : 0u)
{}

LinkSettings &LinkSettings::shared()
{
    static LinkSettings instance;
    return instance;
}

LinkSettings *LinkSettings::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    Q_UNUSED(qmlEngine);
    Q_UNUSED(jsEngine);
    LinkSettings &settings = shared();
    // A function-local static: the engine must not delete it.
    QJSEngine::setObjectOwnership(&settings, QJSEngine::CppOwnership);
    return &settings;
}

void LinkSettings::setEditorCommand(const QString &command)
{
    const QString trimmed = command.trimmed();
    if (trimmed == m_editorCommand) {
        return;
    }
    m_editorCommand = trimmed;
    QSettings().setValue(kCommandKey, trimmed);
    emit editorChanged();
}

void LinkSettings::setEditorInTerminal(bool inTerminal)
{
    if (inTerminal == editorInTerminal()) {
        return;
    }
    m_editorInTerminal = inTerminal ? 1u : 0u;
    QSettings().setValue(kInTerminalKey, inTerminal);
    emit editorChanged();
}

} // namespace konsoll
