#include "WindowStateStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

namespace konsoll
{

namespace
{
QString collectionPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/windows");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/index.json");
}
} // namespace

bool WindowStateStore::s_terminating = false;

QStringList WindowStateStore::listWindowIds() const
{
    const QString path = collectionPath();
    if (!QFile::exists(path)) {
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QByteArray data = file.readAll();
    file.close();
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        return {};
    }

    const QJsonArray ids = doc.object().value(QStringLiteral("ids")).toArray();
    QStringList result;
    result.reserve(ids.size());
    for (const QJsonValue value : ids) {
        result.append(value.toString());
    }

    return result;
}

QString WindowStateStore::generateWindowId() const
{
    return QUuid::createUuid().toString();
}

void WindowStateStore::saveCollection(const QVariant &ids) const
{
    QJsonArray array;
    const auto list = ids.toList();
    for (const QVariant &id : list) {
        array.append(id.toString());
    }

    QJsonObject object;
    object[QStringLiteral("ids")] = array;

    QFile file(collectionPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }

    file.write(QJsonDocument(object).toJson());
    file.close();
}

bool WindowStateStore::terminating() const
{
    return s_terminating;
}

void WindowStateStore::setTerminating(bool terminating)
{
    s_terminating = terminating;
}

} // namespace konsoll
