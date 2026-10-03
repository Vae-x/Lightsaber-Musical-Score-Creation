#include "WorkspacePaths.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace lmsc {
namespace {
bool ensureWritable(const QString &path) {
    if (!QDir().mkpath(path)) return false;
    QTemporaryFile probe(QDir(path).filePath(".write-check-XXXXXX"));
    return probe.open();
}
QString projectName(QString name) {
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), "_");
    name = name.trimmed().left(80);
    while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
    if (name.isEmpty()) name = QStringLiteral("我的曲谱工程");
    const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$"),
                                      QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch()) name.prepend('_');
    return name;
}
}

QString WorkspacePaths::projectsDirectory(const QString &executableDirectory) {
    const QString location = executableDirectory.isEmpty() ? QCoreApplication::applicationDirPath() : executableDirectory;
    QDir cursor(location);
    QString preferred = cursor.filePath("projects");
    if (!QFileInfo(cursor.filePath("package-manifest.json")).isFile()) {
        for (int level = 0; level < 8; ++level) {
            if (QFileInfo(cursor.filePath("CMakeLists.txt")).isFile()
                && QFileInfo(cursor.filePath("src/core/BeatmapDocument.h")).isFile()) {
                preferred = cursor.filePath("projects");
                break;
            }
            if (!cursor.cdUp()) break;
        }
    }
    if (ensureWritable(preferred)) return QDir(preferred).absolutePath();
    const QString fallback = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("光剑曲谱制作/工程"));
    return ensureWritable(fallback) ? QDir(fallback).absolutePath() : QString();
}

QString WorkspacePaths::suggestedProjectFile(const QString &title, const QString &projectsRoot) {
    const QString root = projectsRoot.isEmpty() ? projectsDirectory() : projectsRoot;
    if (root.isEmpty()) return {};
    const QString name = projectName(title);
    QString directory = QDir(root).filePath(name);
    int suffix = 2;
    while (QFileInfo(directory).exists()) directory = QDir(root).filePath(name + "-" + QString::number(suffix++));
    if (!QDir().mkpath(directory)) return {};
    return QDir(directory).filePath("project.lmsc");
}

QString WorkspacePaths::suggestedSongExportFolder(const QString &title, const QString &parent,
                                                  QString *error) {
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QString();
    };
    const QFileInfo parentInfo(parent);
    if (!parentInfo.isDir() || parentInfo.isSymLink() || parentInfo.canonicalFilePath().isEmpty())
        return fail(QStringLiteral("请选择可用的电脑导出目录。"));
    const QString root = QDir(parentInfo.canonicalFilePath()).filePath(QStringLiteral("光剑曲谱制作"));
    const QFileInfo rootInfo(root);
    if (rootInfo.exists() && (!rootInfo.isDir() || rootInfo.isSymLink()))
        return fail(QStringLiteral("导出分类路径已被文件或链接占用，未覆盖已有内容。"));
    if (!ensureWritable(root)) return fail(QStringLiteral("导出分类目录不可写，请选择其他位置。"));
    const QString rootCanonical = QFileInfo(root).canonicalFilePath();
    if (QFileInfo(rootCanonical).absolutePath().compare(parentInfo.canonicalFilePath(), Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("导出分类目录越出所选位置。"));
    const QString name = projectName(title.trimmed().isEmpty() ? QStringLiteral("无题歌曲") : title)
        + QStringLiteral("-by光剑曲谱");
    QString candidate = QDir(rootCanonical).filePath(name);
    int suffix = 2;
    while (QFileInfo::exists(candidate))
        candidate = QDir(rootCanonical).filePath(name + "-" + QString::number(suffix++));
    return candidate;
}
} // namespace lmsc
