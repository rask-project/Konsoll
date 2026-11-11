#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml>

#include <expected>

namespace konsoll
{

// Where the shell-integration scripts are installed (and the generated
// functions files live): <AppConfigLocation>/shell-integration.
QString shellIntegrationDirectory();

// Shell functions defined in Konsoll instead of a shell rc file: each
// {name, description, body} is kept in QSettings and written as
// functions.zsh / functions.bash / functions.fish next to the shell
// integration, which sources them when the shell starts and again at the
// next prompt after they change (functions.serial). Every tab and every
// background command can call them.
class ShellFunctions final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(ShellFunctions)
    QML_SINGLETON

    Q_PROPERTY(QVariantList functions READ functions NOTIFY functionsChanged)
    // The user's shell ("zsh", "bash", "fish"): the language of the bodies.
    Q_PROPERTY(QString shellName READ shellName CONSTANT)

public:
    explicit ShellFunctions(QObject *parent = nullptr);

    // Writes the functions files from the saved functions (at startup, before
    // the first shell starts).
    static std::expected<void, QString> installScripts();

    QVariantList functions() const { return m_functions.toVariantList(); }
    static QString shellName();

    // Why `name` cannot be used for the function at `index` (-1 for a new
    // one), or an empty string when it can.
    Q_INVOKABLE QString validate(int index, const QString &name) const;
    // Adds (index -1) or replaces a function and rewrites the files. Returns
    // an error message, or an empty string on success.
    Q_INVOKABLE QString saveFunction(int index, const QString &name, const QString &description,
                                     const QString &body);
    Q_INVOKABLE QString removeFunction(int index);

signals:
    void functionsChanged();

private:
    QString store();

    QJsonArray m_functions;
};

} // namespace konsoll
