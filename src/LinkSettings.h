#pragma once

#include <QObject>
#include <QString>
#include <QtQml>

#include <cstdint>

namespace konsoll
{

// Settings > Terminal: what opens a file Cmd+clicked in the terminal.
// `editorCommand` (empty = the file's default application) gets the file's
// text files, with {file}, {line} and {column} filled in; with
// `editorInTerminal` it is a terminal editor (nvim, hx) typed into a new
// tab, otherwise a GUI editor's command line (code -g {file}:{line}).
// Saved as General/editorCommand and General/editorInTerminal.
class LinkSettings final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(LinkSettings)
    QML_SINGLETON

    Q_PROPERTY(QString editorCommand READ editorCommand WRITE setEditorCommand NOTIFY editorChanged)
    Q_PROPERTY(bool editorInTerminal READ editorInTerminal WRITE setEditorInTerminal NOTIFY editorChanged)

public:
    // The one instance, shared by the sessions and QML.
    static LinkSettings &shared();
    static LinkSettings *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    QString editorCommand() const { return m_editorCommand; }
    void setEditorCommand(const QString &command);
    bool editorInTerminal() const { return m_editorInTerminal != 0; }
    void setEditorInTerminal(bool inTerminal);

signals:
    void editorChanged();

private:
    LinkSettings();

    QString m_editorCommand;
    std::uint64_t m_editorInTerminal = 0; // pointer-sized: no tail padding
};

} // namespace konsoll
