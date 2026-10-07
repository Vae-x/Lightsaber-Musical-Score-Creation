#include "core/WorkspacePaths.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>

namespace {
int failures = 0;

void printLine(FILE *destination, const QString &message) {
    QTextStream stream(destination);
    stream.setCodec("UTF-8");
    stream << message << '\n';
}

void check(bool condition, const QString &message) {
    if (condition) return;
    ++failures;
    printLine(stderr, QStringLiteral("失败：") + message);
}

bool writeFile(const QString &path, const QByteArray &contents = {}) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool samePath(const QString &first, const QString &second) {
    return QDir::cleanPath(QFileInfo(first).absoluteFilePath())
        == QDir::cleanPath(QFileInfo(second).absoluteFilePath());
}

void sourceAndPortableLocations(const QString &temporaryRoot) {
    const QString documents = QDir(temporaryRoot).filePath("documents-fallback");
    const QString source = QDir(temporaryRoot).filePath("source");
    check(writeFile(QDir(source).filePath("CMakeLists.txt")), QStringLiteral("创建源码根标记"));
    check(writeFile(QDir(source).filePath("src/core/BeatmapDocument.h")), QStringLiteral("创建源码结构标记"));
    const QString build = QDir(source).filePath("build/debug/src/tests");
    check(QDir().mkpath(build), QStringLiteral("创建源码构建目录"));
    const QString sourceProjects = lmsc::WorkspacePaths::projectsDirectory(build, documents);
    check(samePath(sourceProjects, QDir(source).filePath("projects")),
          QStringLiteral("从深层构建目录找到仓库根的 projects"));
    check(QFileInfo(sourceProjects).isDir(), QStringLiteral("默认源码工程目录自动创建"));

    // 便携包即使位于源码仓库内部，也应由自己的清单确定工程位置。
    const QString portable = QDir(source).filePath("dist/portable");
    check(writeFile(QDir(portable).filePath("package-manifest.json"), "{}"),
          QStringLiteral("创建便携包清单"));
    const QString portableProjects = lmsc::WorkspacePaths::projectsDirectory(portable, documents);
    check(samePath(portableProjects, QDir(portable).filePath("projects")),
          QStringLiteral("便携工程保存到清单旁的 projects，不回溯到源码仓库"));
    check(!samePath(sourceProjects, portableProjects), QStringLiteral("源码与便携包默认工程目录各自独立"));

    const QString standalone = QDir(temporaryRoot).filePath("standalone");
    check(QDir().mkpath(standalone), QStringLiteral("创建独立程序目录"));
    check(samePath(lmsc::WorkspacePaths::projectsDirectory(standalone, documents),
                   QDir(standalone).filePath("projects")),
          QStringLiteral("没有仓库标记时使用程序旁的 projects"));

    const QString blockedPortable = QDir(temporaryRoot).filePath("blocked-portable");
    check(writeFile(QDir(blockedPortable).filePath("projects"), "keep existing file"),
          QStringLiteral("模拟程序旁的工程根被普通文件占用"));
    check(samePath(lmsc::WorkspacePaths::projectsDirectory(blockedPortable, documents),
                   QDir(documents).filePath(QStringLiteral("光剑曲谱制作/工程"))),
          QStringLiteral("便携工程目录不可写时仍回退文档目录"));
    check(readFile(QDir(blockedPortable).filePath("projects")) == "keep existing file",
          QStringLiteral("回退不覆盖程序旁占用路径的文件"));
}

void installedLocations(const QString &temporaryRoot) {
    const QString documents = QDir(temporaryRoot).filePath("documents");
    const QString expected = QDir(documents).filePath(QStringLiteral("光剑曲谱制作/工程"));
    const QString installed = QDir(temporaryRoot).filePath("programs/installed");
    check(writeFile(QDir(installed).filePath("installed-mode.ini"), "[Deployment]\nMode=Installed\n"),
          QStringLiteral("创建安装版固定标记"));
    check(writeFile(QDir(installed).filePath("package-manifest.json"), "{}"),
          QStringLiteral("安装版保留发布清单"));
    check(writeFile(QDir(installed).filePath("writable-check"), "writable"),
          QStringLiteral("模拟安装目录确实可写"));
    check(samePath(lmsc::WorkspacePaths::projectsDirectory(installed, documents), expected),
          QStringLiteral("安装目录可写时仍将工程放到文档目录"));
    check(QFileInfo(expected).isDir(), QStringLiteral("安装版建立文档工程根目录"));
    check(!QFileInfo::exists(QDir(installed).filePath("projects")),
          QStringLiteral("安装版不在程序目录创建 projects"));

    const QList<QByteArray> invalidMarkers = {"[Deployment]\nMode=Portable\n", "[Deployment]\nMode=installed\n",
        "[Other]\nMode=Installed\n", "[Deployment]\n", "[Deployment]\nMode=@ByteArray(Installed)\n", ""};
    for (int index = 0; index < invalidMarkers.size(); ++index) {
        const QString portable = QDir(temporaryRoot).filePath("invalid-marker-" + QString::number(index));
        check(writeFile(QDir(portable).filePath("installed-mode.ini"), invalidMarkers.at(index)),
              QStringLiteral("创建无效安装标记"));
        check(samePath(lmsc::WorkspacePaths::projectsDirectory(portable, documents),
                       QDir(portable).filePath("projects")),
              QStringLiteral("未知值、错误分组与非字符串标记保持便携版路径：") + QString::number(index));
    }

    const QString directoryMarker = QDir(temporaryRoot).filePath("directory-marker");
    check(QDir().mkpath(QDir(directoryMarker).filePath("installed-mode.ini")),
          QStringLiteral("模拟同名目录占用安装标记"));
    check(samePath(lmsc::WorkspacePaths::projectsDirectory(directoryMarker, documents),
                   QDir(directoryMarker).filePath("projects")),
          QStringLiteral("同名目录不能作为安装版标记"));

    const QString nestedPortable = QDir(installed).filePath("nested-portable");
    check(QDir().mkpath(nestedPortable), QStringLiteral("创建安装目录下的独立便携目录"));
    check(samePath(lmsc::WorkspacePaths::projectsDirectory(nestedPortable, documents),
                   QDir(nestedPortable).filePath("projects")),
          QStringLiteral("安装标记只识别程序旁的固定文件，不继承上级标记"));

    const QString blockedDocuments = QDir(temporaryRoot).filePath("blocked-documents");
    check(writeFile(blockedDocuments, "keep existing file"), QStringLiteral("模拟不可用文档目录"));
    check(lmsc::WorkspacePaths::projectsDirectory(installed, blockedDocuments).isEmpty(),
          QStringLiteral("安装版文档工程目录不可用时报告失败"));
    check(!QFileInfo::exists(QDir(installed).filePath("projects")),
          QStringLiteral("文档目录失败时也不回写安装目录"));
    check(readFile(blockedDocuments) == "keep existing file", QStringLiteral("保留占用文档路径的原文件"));

    const QString previousCurrent = QDir::currentPath();
    check(QDir::setCurrent(installed), QStringLiteral("隔离当前工作目录到临时安装目录"));
    for (const QString &relativeDocuments : {QStringLiteral("."), QStringLiteral("relative-documents")}) {
        check(lmsc::WorkspacePaths::projectsDirectory(installed, relativeDocuments).isEmpty(),
              QStringLiteral("安装版拒绝相对文档目录：") + relativeDocuments);
    }
    check(!QFileInfo::exists(QDir(installed).filePath(QStringLiteral("光剑曲谱制作")))
          && !QFileInfo::exists(QDir(installed).filePath("relative-documents"))
          && !QFileInfo::exists(QDir(installed).filePath("projects")),
          QStringLiteral("拒绝相对文档路径时不写安装目录或当前工作目录"));
    check(QDir::setCurrent(previousCurrent), QStringLiteral("恢复路径测试前的工作目录"));
}

void safeNamesAndContainment(const QString &root) {
    check(QDir().mkpath(root), QStringLiteral("创建安全路径测试目录"));
    const QString chinese = lmsc::WorkspacePaths::suggestedProjectFile(QStringLiteral("星穹绿洲·中文歌曲"), root);
    check(samePath(chinese, QDir(root).filePath(QStringLiteral("星穹绿洲·中文歌曲/project.lmsc"))),
          QStringLiteral("中文歌曲名保持可读"));
    const QString englishTitle = QStringLiteral("Standard Chart - mix01.fx");
    const QString english = lmsc::WorkspacePaths::suggestedProjectFile(englishTitle, root);
    check(samePath(english, QDir(root).filePath(englishTitle + "/project.lmsc")),
          QStringLiteral("合法的英文字母、数字、连字符和扩展名不被误替换"));

    QString controlTitle = QStringLiteral("../坏\\路径<>:\"|?*");
    controlTitle.append(QChar(0));
    controlTitle.append(QChar(31));
    const QStringList titles = {controlTitle, QStringLiteral("CON"), QStringLiteral("prn"),
        QStringLiteral("AUX.txt"), QStringLiteral("nul"), QStringLiteral("COM1"), QStringLiteral("com9.wav"),
        QStringLiteral("LPT1"), QStringLiteral("lpt9.dat"), QStringLiteral("  尾部空格与点.  "),
        QStringLiteral("."), QStringLiteral(".."), QString(), QString(200, QChar(0x6b4c))};
    const QRegularExpression forbidden(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$"),
                                      QRegularExpression::CaseInsensitiveOption);
    for (const QString &title : titles) {
        const QString project = lmsc::WorkspacePaths::suggestedProjectFile(title, root);
        const QFileInfo file(project);
        const QDir directory(file.absolutePath());
        const QString name = QFileInfo(directory.absolutePath()).fileName();
        check(!project.isEmpty(), QStringLiteral("异常歌曲名仍能生成默认工程路径"));
        check(file.fileName() == "project.lmsc", QStringLiteral("默认入口始终为 project.lmsc"));
        check(samePath(QFileInfo(directory.absolutePath()).absolutePath(), root),
              QStringLiteral("歌曲目录不会通过斜杠或 .. 逃出工程根目录"));
        check(!forbidden.match(name).hasMatch() && !reserved.match(name).hasMatch(),
              QStringLiteral("工程目录不含 Windows 非法字符或保留设备名：") + name);
        check(!name.isEmpty() && !name.endsWith('.') && !name.endsWith(' ') && name.size() <= 80,
              QStringLiteral("工程目录名非空、长度受限且无不安全尾字符"));
        check(directory.exists(), QStringLiteral("建议路径对应的工程目录已创建"));
    }
}

void sameNameDoesNotOverwrite(const QString &root) {
    check(QDir().mkpath(root), QStringLiteral("创建同名工程测试目录"));
    const QString title = QStringLiteral("同名歌曲");
    const QByteArray original("existing project must remain intact");
    const QString first = lmsc::WorkspacePaths::suggestedProjectFile(title, root);
    check(writeFile(first, original), QStringLiteral("写入已有工程哨兵数据"));
    const QString second = lmsc::WorkspacePaths::suggestedProjectFile(title, root);
    check(samePath(second, QDir(root).filePath(title + "-2/project.lmsc")),
          QStringLiteral("同名工程自动生成新目录"));
    check(readFile(first) == original, QStringLiteral("建议新路径不覆盖已有工程内容"));

    // 已存在的普通文件同样不能被当作目录覆盖。
    const QString occupiedFile = QDir(root).filePath(title + "-3");
    const QByteArray occupied("keep this file");
    check(writeFile(occupiedFile, occupied), QStringLiteral("创建占用路径的普通文件"));
    const QString third = lmsc::WorkspacePaths::suggestedProjectFile(title, root);
    check(samePath(third, QDir(root).filePath(title + "-4/project.lmsc")),
          QStringLiteral("同名候选路径存在普通文件时继续选择空闲目录"));
    check(readFile(occupiedFile) == occupied && readFile(first) == original,
          QStringLiteral("同名选择过程保留已有文件和工程"));

    const QString blockedRoot = QDir(root).filePath("not-a-directory");
    check(writeFile(blockedRoot, occupied), QStringLiteral("创建无法作为根目录的路径"));
    check(lmsc::WorkspacePaths::suggestedProjectFile(title, blockedRoot).isEmpty(),
          QStringLiteral("显式工程根不是目录时返回失败，不覆盖文件"));
    check(readFile(blockedRoot) == occupied, QStringLiteral("不可用工程根的原文件保留"));
}

void songExportNames(const QString &root) {
    check(QDir().mkpath(root), QStringLiteral("创建歌曲导出测试根"));
    QString error;
    const QString first = lmsc::WorkspacePaths::suggestedSongExportFolder(QStringLiteral("诀别书"), root, &error);
    check(samePath(first, QDir(root).filePath(QStringLiteral("光剑曲谱制作/诀别书-by光剑曲谱"))),
          QStringLiteral("歌曲导出使用分类目录与指定命名"));
    check(!QFileInfo::exists(first), QStringLiteral("只建议歌曲路径，保留原子导出创建目标的机会"));
    const QByteArray sentinel("original export remains intact");
    check(writeFile(QDir(first).filePath("Info.dat"), sentinel), QStringLiteral("创建已有导出哨兵"));
    const QString second = lmsc::WorkspacePaths::suggestedSongExportFolder(QStringLiteral("诀别书"), root, &error);
    check(second == first + "-2" && readFile(QDir(first).filePath("Info.dat")) == sentinel,
          QStringLiteral("重名使用编号并保留既有导出"));
    check(writeFile(second, sentinel), QStringLiteral("普通文件占用同名输出"));
    check(lmsc::WorkspacePaths::suggestedSongExportFolder(QStringLiteral("诀别书"), root, &error) == first + "-3",
          QStringLiteral("被普通文件占用的名称同样跳过"));
    for (const QString &title : {QString(), QStringLiteral("CON"), QStringLiteral("../坏\\路径<>:*? ."), QString(200, QChar(0x6b4c))}) {
        const QString path = lmsc::WorkspacePaths::suggestedSongExportFolder(title, root, &error);
        const QString filename = QFileInfo(path).fileName();
        check(!path.isEmpty() && filename.endsWith(QStringLiteral("-by光剑曲谱"))
              && QFileInfo(path).absolutePath() == QFileInfo(first).absolutePath()
              && !filename.contains('/') && !filename.contains('\\') && filename.size() <= 89,
              QStringLiteral("异常歌名安全处理且保留导出后缀"));
    }
    const QString blocked = QDir(root).filePath("blocked");
    check(QDir().mkpath(blocked) && writeFile(QDir(blocked).filePath(QStringLiteral("光剑曲谱制作")), sentinel),
          QStringLiteral("分类目录被文件占用"));
    check(lmsc::WorkspacePaths::suggestedSongExportFolder("test", blocked, &error).isEmpty()
          && !error.isEmpty() && readFile(QDir(blocked).filePath(QStringLiteral("光剑曲谱制作"))) == sentinel,
          QStringLiteral("分类占用清晰失败且不覆盖"));
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        printLine(stderr, QStringLiteral("失败：无法创建临时测试目录"));
        return 1;
    }
    sourceAndPortableLocations(temporary.path());
    installedLocations(QDir(temporary.path()).filePath("installed"));
    songExportNames(QDir(temporary.path()).filePath("export"));
    safeNamesAndContainment(QDir(temporary.path()).filePath("safe-names"));
    sameNameDoesNotOverwrite(QDir(temporary.path()).filePath("same-name"));
    printLine(stdout, failures ? QStringLiteral("工程路径验证失败：") + QString::number(failures)
                             : QStringLiteral("通过：源码、便携与安装工程位置、安装标记、中文和非法名称、路径范围、同名工程保护"));
    return failures ? 1 : 0;
}
