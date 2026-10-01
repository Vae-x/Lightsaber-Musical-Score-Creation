#include "ProjectStore.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QRegularExpression>

namespace lmsc {
namespace {
bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
bool inside(const QString &path, const QString &root) {
    const QString normalized = QDir::fromNativeSeparators(path);
    const QString base = QDir::fromNativeSeparators(root);
    return normalized.startsWith(base + '/', Qt::CaseInsensitive);
}
}

bool ProjectStore::safeRelativePath(const QString &relative) {
    const QString path = QDir::fromNativeSeparators(relative);
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(':') ||
        path.contains(QChar::Null)) return false;
    const auto parts = path.split('/', QString::KeepEmptyParts);
    const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$"),
                                      QRegularExpression::CaseInsensitiveOption);
    for (const auto &part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') ||
            part.endsWith(' ') || reserved.match(part).hasMatch()) return false;
        for (const auto character : part)
            if (character.unicode() < 32 || QStringLiteral("<>\"|?*").contains(character)) return false;
    }
    return true;
}

QStringList ProjectStore::files(const QString &root, QString *error) {
    const QFileInfo rootInfo(root);
    const QString base = rootInfo.canonicalFilePath();
    if (!rootInfo.isDir() || base.isEmpty() || rootInfo.isSymLink()) {
        fail(error, QStringLiteral("找不到安全的歌曲目录：%1").arg(root));
        return {};
    }
    QStringList result;
    QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const auto info = it.fileInfo();
        const QString canonical = info.canonicalFilePath();
        const QString relative = QDir(root).relativeFilePath(info.absoluteFilePath());
        if (info.isSymLink() || canonical.isEmpty() || !inside(canonical, base) ||
            !safeRelativePath(relative)) {
            fail(error, QStringLiteral("目录含不安全的路径或链接：%1").arg(relative));
            return {};
        }
        if (info.isFile()) result.append(relative);
    }
    result.sort(Qt::CaseInsensitive);
    return result;
}

bool ProjectStore::copyTree(const QString &source, const QString &destination, QString *error) {
    QString listError;
    const auto list = files(source, &listError);
    if (!listError.isEmpty()) return fail(error, listError);
    const QString srcCanonical = QFileInfo(source).canonicalFilePath();
    const QString dstAbsolute = QDir::cleanPath(QFileInfo(destination).absoluteFilePath());
    if (dstAbsolute.compare(srcCanonical, Qt::CaseInsensitive) == 0 || inside(dstAbsolute, srcCanonical))
        return fail(error, QStringLiteral("复制目标不能位于源歌曲目录中。"));
    if (!QDir().mkpath(destination)) return fail(error, QStringLiteral("无法建立目标目录：%1").arg(destination));
    const QString dstCanonical = QFileInfo(destination).canonicalFilePath();
    for (const auto &relative : list) {
        const QString src = QDir(source).filePath(relative);
        const QString dst = QDir(destination).filePath(relative);
        if (!QDir().mkpath(QFileInfo(dst).absolutePath()))
            return fail(error, QStringLiteral("无法建立资源子目录：%1").arg(relative));
        const QString parent = QFileInfo(QFileInfo(dst).absolutePath()).canonicalFilePath();
        if (parent != dstCanonical && !inside(parent, dstCanonical))
            return fail(error, QStringLiteral("复制目标路径越出工程目录。"));
        if (QFileInfo::exists(dst)) return fail(error, QStringLiteral("目标已有文件，未覆盖：%1").arg(dst));
        if (!QFile::copy(src, dst)) return fail(error, QStringLiteral("无法复制资源：%1").arg(relative));
    }
    return true;
}

bool ProjectStore::writeJson(const QString &path, const QJsonObject &json, QString *error) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) return fail(error, file.errorString());
    return true;
}

bool ProjectStore::readJson(const QString &path, QJsonObject *json, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
    // Keep malformed or hostile metadata from exhausting the 32-bit process.
    if (file.size() > 64 * 1024 * 1024)
        return fail(error, QStringLiteral("JSON 超过首版的 64 MiB 读取上限：%1").arg(path));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("JSON 格式错误 %1：%2").arg(path, parse.errorString()));
    *json = document.object();
    return true;
}

bool ProjectStore::extractZip(const QString &archive, const QString &destination, QString *error) {
#ifndef Q_OS_WIN
    Q_UNUSED(archive)
    Q_UNUSED(destination)
    return fail(error, QStringLiteral("首版 ZIP 导入使用 Windows 内置解压服务。"));
#else
    if (!QFileInfo(archive).isFile() || !QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
        return fail(error, QStringLiteral("ZIP 不存在或解压目标不是空目录。"));
    if (!QDir().mkpath(destination)) return fail(error, QStringLiteral("无法建立 ZIP 导入目录。"));
    // Paths are process-local environment variables, never concatenated into shell commands.
    const QString script = QString::fromLatin1(R"PS(
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding
[Console]::InputEncoding = New-Object System.Text.UTF8Encoding
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = $null
try {
  $base = [System.IO.Path]::GetFullPath($env:LMSC_ZIP_DEST)
  $prefix = $base.TrimEnd('\') + '\'
  $zip = [System.IO.Compression.ZipFile]::OpenRead($env:LMSC_ZIP_SOURCE)
  if ($zip.Entries.Count -gt 10000) { throw 'Archive has too many entries.' }
  $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
  [long]$sum = 0
  foreach ($entry in $zip.Entries) {
    $name = $entry.FullName.Replace('\', '/')
    if ($name.EndsWith('/')) { $name = $name.TrimEnd('/') }
    if (!$name -or [System.IO.Path]::IsPathRooted($name) -or $name.Contains(':')) { throw 'Unsafe archive path.' }
    foreach ($part in $name.Split('/')) {
      if (!$part -or $part -eq '.' -or $part -eq '..' -or $part -match '[<>:"|?*\x00-\x1f]' -or
          $part.EndsWith('.') -or $part.EndsWith(' ') -or $part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?$') {
        throw 'Unsafe archive component.'
      }
    }
    $target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($base, $name))
    if (!$target.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Archive escapes destination.' }
    if (!$seen.Add($name)) { throw 'Duplicate archive path.' }
    if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Archive contains a symbolic link.' }
    $sum += $entry.Length
    if ($sum -gt 2147483648 -or $entry.Length -gt 1073741824) { throw 'Archive exceeds the import size limit.' }
  }
  foreach ($entry in $zip.Entries) {
    $target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($base, $entry.FullName.Replace('\', '/')))
    if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')) {
      [System.IO.Directory]::CreateDirectory($target) | Out-Null
    } else {
      [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($target)) | Out-Null
      [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $false)
    }
  }
} catch {
  [Console]::Error.WriteLine($_.Exception.Message)
  exit 1
} finally { if ($zip) { $zip.Dispose() } }
)PS");
    QByteArray utf16;
    utf16.reserve(script.size() * 2);
    for (const auto ch : script) {
        utf16.append(char(ch.unicode() & 0xff));
        utf16.append(char(ch.unicode() >> 8));
    }
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LMSC_ZIP_SOURCE"), QFileInfo(archive).absoluteFilePath());
    environment.insert(QStringLiteral("LMSC_ZIP_DEST"), QFileInfo(destination).absoluteFilePath());
    process.setProcessEnvironment(environment);
    const QString powershell = QDir(environment.value(QStringLiteral("SystemRoot"), QStringLiteral("C:/Windows")))
            .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    process.start(powershell, {QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
                              QStringLiteral("-NonInteractive"), QStringLiteral("-EncodedCommand"),
                              QString::fromLatin1(utf16.toBase64())});
    if (!process.waitForStarted(10000)) return fail(error, QStringLiteral("无法启动 Windows ZIP 服务：%1").arg(process.errorString()));
    if (!process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished(5000);
        return fail(error, QStringLiteral("ZIP 导入超时。"));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return fail(error, QStringLiteral("ZIP 导入被拒绝：%1").arg(QString::fromUtf8(process.readAllStandardError()).trimmed()));
    return true;
#endif
}

} // namespace lmsc
