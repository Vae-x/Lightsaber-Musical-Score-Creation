#pragma once

#include <QString>
#include <QStringList>

namespace lmsc {

// Android's document URIs are never passed to QFile or interpreted as paths.
// Imports become independent local copies; exports only create new directories.
class AndroidStorage {
public:
    static QString pickInputFile(const QStringList &mimeTypes = {}, QString *error = nullptr);
    static QString pickDirectory(QString *error = nullptr);
    static QString pickSongDirectory(QString *error = nullptr);
    static QString copyDirectoryToTree(const QString &sourceFolder, const QString &treeUri,
                                       QString *error = nullptr);
    static QString copyProjectDirectoryToTree(const QString &sourceFolder, const QString &treeUri,
                                              QString *error = nullptr);
    static bool extractZip(const QString &archive, const QString &destination,
                           QString *error = nullptr);
    static bool protectKey(const QString &key, QString *protectedKey);
    static bool unprotectKey(const QString &protectedKey, QString *key);
};

} // namespace lmsc
