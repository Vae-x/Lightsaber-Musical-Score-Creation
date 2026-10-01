#pragma once

#include <QJsonObject>
#include <QStringList>

namespace lmsc {

// Filesystem helpers keep large assets on disk and reject escapes/symlinks.
class ProjectStore {
public:
    static bool safeRelativePath(const QString &relative);
    static bool copyTree(const QString &source, const QString &destination,
                         QString *error = nullptr);
    static bool writeJson(const QString &path, const QJsonObject &json,
                          QString *error = nullptr);
    static bool readJson(const QString &path, QJsonObject *json,
                         QString *error = nullptr);
    static bool extractZip(const QString &archive, const QString &destination,
                           QString *error = nullptr);
    static QStringList files(const QString &root, QString *error = nullptr);
};

} // namespace lmsc
