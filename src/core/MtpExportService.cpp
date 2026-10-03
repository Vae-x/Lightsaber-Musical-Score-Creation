#include "MtpExportService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QtConcurrent>
#include <functional>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
bool safeName(const QString &name) {
    static const QRegularExpression invalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..")
        && !name.endsWith(QLatin1Char('.')) && !name.endsWith(QLatin1Char(' '))
        && !invalid.match(name).hasMatch() && !reserved.match(name).hasMatch();
}
bool safeRelative(const QString &path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(QLatin1Char('\\'))) return false;
    for (const auto &part : path.split(QLatin1Char('/'))) if (!safeName(part)) return false;
    return true;
}
bool validDestination(const MtpExportDestination &entry) {
    return !entry.deviceId.isEmpty() && !entry.storageId.isEmpty() && !entry.rootId.isEmpty()
        && (entry.gameId == QStringLiteral("oasis") || entry.gameId == QStringLiteral("lightband"));
}
bool manifest(const QString &folder, QJsonArray *files, QJsonArray *references, QString *error, const std::shared_ptr<std::atomic_bool> &stop) {
    const QFileInfo root(folder);
    static const QRegularExpression exportName(QStringLiteral("^.+-by光剑曲谱(?:-(?:[2-9]|[1-9][0-9]+))?$"));
    if (!root.isDir() || root.isSymLink() || !safeName(root.fileName())
            || !exportName.match(root.fileName()).hasMatch()
            || QDir(root.absolutePath()).dirName() != QStringLiteral("光剑曲谱制作")) {
        *error = QStringLiteral("请选择完整导出的 光剑曲谱制作/歌曲名-by光剑曲谱 文件夹，不能上传工程的 assets-* 资源目录。"); return false;
    }
    const QString canonical = root.canonicalFilePath();
    QSet<QString> names;
    qint64 total = 0;
    std::function<bool(const QString &, int)> walk = [&](const QString &relative, int depth) {
        if (stop->load()) return false;
        if (depth > 20) { *error = QStringLiteral("歌曲资源子目录超过 20 层。"); return false; }
        const auto children = QDir(QDir(folder).filePath(relative)).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const auto &child : children) {
            if (stop->load()) return false;
            const QString path = relative.isEmpty() ? child.fileName() : relative + QLatin1Char('/') + child.fileName();
            if (!safeRelative(path) || child.isSymLink() || !child.canonicalFilePath().startsWith(canonical + QLatin1Char('/'), Qt::CaseInsensitive)) {
                *error = QStringLiteral("歌曲含不安全路径或链接：%1").arg(path); return false;
            }
            const QString lower = path.toLower();
            if (names.contains(lower)) { *error = QStringLiteral("歌曲含不区分大小写的重名资源：%1").arg(path); return false; }
            names.insert(lower);
            if (child.isDir()) { if (!walk(path, depth + 1)) return false; continue; }
            if (!child.isFile() || child.suffix().compare(QStringLiteral("lmsc"), Qt::CaseInsensitive) == 0 || files->size() >= 4000) {
                *error = QStringLiteral("歌曲含工程入口、非普通文件或超过 4000 个文件。"); return false;
            }
            total += child.size();
            if (total > 4LL * 1024 * 1024 * 1024) { *error = QStringLiteral("单首歌曲超过 4 GiB 上传限制。"); return false; }
            QFile file(child.absoluteFilePath());
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!file.open(QIODevice::ReadOnly)) { *error = QStringLiteral("无法读取歌曲资源：%1").arg(path); return false; }
            while (!file.atEnd()) {
                if (stop->load()) return false;
                const auto bytes = file.read(65536);
                if (bytes.isEmpty() && file.error() != QFile::NoError) { *error = QStringLiteral("无法读取歌曲资源：%1").arg(path); return false; }
                hash.addData(bytes);
            }
            files->append(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("size"), double(child.size())},
                                     {QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex())}});
        }
        return true;
    };
    if (!walk(QString(), 0)) return false;
    QFile info(QDir(folder).filePath(QStringLiteral("Info.dat")));
    if (!info.open(QIODevice::ReadOnly) || info.size() > 16 * 1024 * 1024) { *error = QStringLiteral("导出歌曲缺少有效的 Info.dat。"); return false; }
    const auto document = QJsonDocument::fromJson(info.readAll());
    if (!document.isObject()) { *error = QStringLiteral("Info.dat 不是有效的 JSON 对象。"); return false; }
    const QSet<QString> referenceKeys{QStringLiteral("_songFilename"), QStringLiteral("songFilename"), QStringLiteral("_coverImageFilename"),
        QStringLiteral("coverImageFilename"), QStringLiteral("_beatmapFilename"), QStringLiteral("beatmapFilename"),
        QStringLiteral("beatmapDataFilename"), QStringLiteral("lightshowDataFilename"), QStringLiteral("audioDataFilename")};
    bool audio = false, beatmap = false, valid = true;
    std::function<void(const QJsonValue &)> collect = [&](const QJsonValue &value) {
        if (value.isArray()) { for (const auto &item : value.toArray()) collect(item); return; }
        if (!value.isObject()) return;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!referenceKeys.contains(it.key())) { collect(it.value()); continue; }
            if (!it.value().isString()) { valid = false; continue; }
            QString path = it.value().toString();
            const bool cover = it.key().contains(QStringLiteral("coverImage"));
            if (path.isEmpty() && cover) continue;
            // PICO 的现有歌曲常以 .egg 声明 .ogg 资源；沿用只读导入的别名规则，保留 Info 原文。
            if (it.key().contains(QStringLiteral("songFilename")) && path.endsWith(QStringLiteral(".egg"), Qt::CaseInsensitive)
                    && !names.contains(path.toLower())) {
                const QString alternate = path.left(path.size() - 4) + QStringLiteral(".ogg");
                if (names.contains(alternate.toLower())) path = alternate;
                else {
                    const auto candidates = QDir(folder).entryList({QStringLiteral("*.ogg")}, QDir::Files);
                    if (candidates.size() == 1) path = candidates.first();
                }
            }
            if (!safeRelative(path) || !names.contains(path.toLower()) || !QFileInfo(QDir(folder).filePath(path)).isFile()) { valid = false; continue; }
            if (it.key().contains(QStringLiteral("songFilename"))) audio = true;
            if (it.key().contains(QStringLiteral("beatmap"), Qt::CaseInsensitive)) beatmap = true;
            references->append(path);
        }
    };
    collect(document.object());
    if (!valid || !audio || !beatmap) { *error = QStringLiteral("Info.dat 含不安全、缺失的声音或谱面引用，请先重新导出完整歌曲。"); return false; }
    return true;
}
}

MtpExportService::MtpExportService(QObject *parent) : QObject(parent) {
    qRegisterMetaType<MtpExportDestination>(); qRegisterMetaType<QVector<MtpExportDestination>>();
#ifdef Q_OS_WIN
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW; arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW; arguments->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
    m_timeout.setSingleShot(true); m_cancelDeadline.setSingleShot(true);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &MtpExportService::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { m_process.readAllStandardError(); });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MtpExportService::finish);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !isBusy()) return;
        m_timeout.stop(); m_cancelDeadline.stop(); m_operation = Operation::None;
        emit errorOccurred(QStringLiteral("无法启动设备导出工具：%1").arg(m_process.errorString()));
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] { requestStop(true); });
    connect(&m_cancelDeadline, &QTimer::timeout, this, [this] {
        if (m_process.state() != QProcess::NotRunning) m_process.kill();
    });
    connect(&m_preparer, &QFutureWatcher<ManifestResult>::finished, this, [this] {
        const auto result = m_preparer.result(); m_preparing = false;
        if (m_cancelled || m_timedOut) {
            m_operation = Operation::None;
            if (m_timedOut) emit errorOccurred(QStringLiteral("校验电脑歌曲超时，未向设备写入任何歌曲。")); else emit cancelled();
            return;
        }
        if (!result.error.isEmpty()) {
            m_timeout.stop(); m_operation = Operation::None; emit errorOccurred(result.error); return;
        }
        launch(result.files, result.references);
    });
}
MtpExportService::~MtpExportService() {
    m_process.disconnect(this); m_timeout.stop(); m_cancelDeadline.stop();
    m_preparer.disconnect(this);
    if (m_stopPreparation) m_stopPreparation->store(true);
    m_preparer.waitForFinished();
    if (m_process.state() != QProcess::NotRunning) {
        QFile marker(m_cancelFile); if (marker.open(QIODevice::WriteOnly)) marker.write("cancel");
        if (!m_process.waitForFinished(3000)) { m_process.kill(); m_process.waitForFinished(3000); }
    }
}
void MtpExportService::setHelperPath(const QString &path) { m_helper = path; }
QString MtpExportService::helperPath() const {
    if (!m_helper.isEmpty()) return m_helper;
    const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/tools/mtp/WpdTransfer.exe");
    if (QFileInfo::exists(bundled)) return bundled;
    QDir directory(QCoreApplication::applicationDirPath());
    for (int level = 0; level < 8; ++level) {
        const QString sibling = directory.filePath(QStringLiteral("tools/mtp/WpdTransfer.exe"));
        if (QFileInfo::exists(sibling)) return sibling;
        const QString development = directory.filePath(QStringLiteral("build/tools/mtp/WpdTransfer.exe"));
        if (QFileInfo::exists(development)) return development;
        if (!directory.cdUp()) break;
    }
    return {};
}
bool MtpExportService::isBusy() const { return m_operation != Operation::None; }
void MtpExportService::listDestinations() { start(Operation::List); }
void MtpExportService::uploadSong(const QString &folder, const MtpExportDestination &destination) {
    if (!validDestination(destination)) { emit errorOccurred(QStringLiteral("设备导出目标无效，请重新刷新设备。") ); return; }
    start(Operation::Upload, folder, destination);
}
void MtpExportService::start(Operation operation, const QString &folder, const MtpExportDestination &destination) {
    if (isBusy()) { emit errorOccurred(QStringLiteral("正在进行设备导出，请等待或取消。")); return; }
#ifndef Q_OS_WIN
    Q_UNUSED(operation) Q_UNUSED(folder) Q_UNUSED(destination)
    emit errorOccurred(QStringLiteral("设备直接导出目前仅支持 Windows。")); return;
#endif
    const QString helper = helperPath();
    if (!QFileInfo(helper).isFile()) { emit errorOccurred(QStringLiteral("缺少 tools/mtp/WpdTransfer.exe，请重新解压完整便携包。")); return; }
    m_folder = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    m_pendingHelper = helper; m_destination = destination;
    m_operation = operation; m_cancelled = m_timedOut = m_done = false; m_preparing = false;
    m_error.clear(); m_stdout.clear(); m_destinations.clear(); m_location.clear(); m_cancelFile.clear();
    m_timeout.start(operation == Operation::List ? 120000 : 900000);
    if (operation == Operation::Upload) {
        m_preparing = true; m_stopPreparation.reset(new std::atomic_bool(false));
        const auto stop = m_stopPreparation; const QString localFolder = m_folder;
        m_preparer.setFuture(QtConcurrent::run([localFolder, stop] {
            ManifestResult result;
            if (!manifest(localFolder, &result.files, &result.references, &result.error, stop) && result.error.isEmpty() && !stop->load())
                result.error = QStringLiteral("无法校验电脑导出歌曲。");
            return result;
        }));
        emit taskProgress(QStringLiteral("校验电脑歌曲并准备设备导出"), -1);
    } else launch();
}
void MtpExportService::launch(const QJsonArray &files, const QJsonArray &references) {
    const auto fail = [this](const QString &message) { m_timeout.stop(); m_operation = Operation::None; emit errorOccurred(message); };
    m_session.reset(new QTemporaryDir(QDir::tempPath() + QStringLiteral("/lmsc-wpd-XXXXXX")));
    if (!m_session->isValid()) { fail(QStringLiteral("无法创建设备导出任务目录。")); return; }
    m_cancelFile = m_session->filePath(QStringLiteral("cancel"));
    const QString jobPath = m_session->filePath(QStringLiteral("job.json"));
    const QJsonObject job{{QStringLiteral("protocol"), 1}, {QStringLiteral("mode"), m_operation == Operation::List ? QStringLiteral("list") : QStringLiteral("upload")},
        {QStringLiteral("cancelFile"), m_cancelFile}, {QStringLiteral("localFolder"), m_folder}, {QStringLiteral("files"), files}, {QStringLiteral("references"), references},
        {QStringLiteral("destination"), QJsonObject{{QStringLiteral("deviceId"), m_destination.deviceId}, {QStringLiteral("storageId"), m_destination.storageId},
            {QStringLiteral("rootId"), m_destination.rootId}, {QStringLiteral("gameId"), m_destination.gameId}}}};
    QFile file(jobPath);
    const auto data = QJsonDocument(job).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) { fail(QStringLiteral("无法写入设备导出任务。")); return; }
    file.close();
    const QString helper = m_pendingHelper;
    if (QFileInfo(helper).suffix().compare(QStringLiteral("ps1"), Qt::CaseInsensitive) == 0) {
        const QString windows = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
        QString powershell = windows + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
#if defined(Q_OS_WIN) && !defined(_WIN64)
        const QString native = windows + QStringLiteral("/Sysnative/WindowsPowerShell/v1.0/powershell.exe");
        if (QFileInfo::exists(native)) powershell = native;
#endif
        m_process.setProgram(powershell);
        m_process.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-File"), QDir::toNativeSeparators(helper), QStringLiteral("-JobPath"), QDir::toNativeSeparators(jobPath)});
    } else {
        m_process.setProgram(helper); m_process.setArguments({QStringLiteral("--job"), QDir::toNativeSeparators(jobPath)});
    }
    emit taskProgress(m_operation == Operation::List ? QStringLiteral("查找头显中的游戏目录") : QStringLiteral("本机歌曲已校验，准备连接设备"), -1);
    if (!isBusy() || m_cancelled) return;
    m_process.start();
}
void MtpExportService::cancel() { if (isBusy()) requestStop(false); }
void MtpExportService::requestStop(bool timeout) {
    if (m_cancelled || m_timedOut) return;
    m_timeout.stop(); m_timedOut = timeout; m_cancelled = !timeout;
    if (m_preparing) {
        m_stopPreparation->store(true);
        emit taskProgress(QStringLiteral("正在取消电脑歌曲校验，尚未向设备写入"), -1);
        return;
    }
    QFile marker(m_cancelFile); if (marker.open(QIODevice::WriteOnly)) marker.write("cancel");
    if (m_process.state() == QProcess::NotRunning) {
        m_operation = Operation::None;
        if (timeout) emit errorOccurred(QStringLiteral("设备导出超时。")); else emit cancelled();
        return;
    }
    emit taskProgress(QStringLiteral("正在取消，等待设备撤销当前文件传输"), -1);
    m_cancelDeadline.start(8000);
}
void MtpExportService::readOutput() {
    m_stdout += m_process.readAllStandardOutput();
    int newline;
    while ((newline = m_stdout.indexOf('\n')) >= 0) {
        const auto line = m_stdout.left(newline).trimmed(); m_stdout.remove(0, newline + 1); consumeLine(line);
    }
    if (m_stdout.size() > 2 * 1024 * 1024) { m_error = QStringLiteral("设备导出工具返回数据超过限制。"); requestStop(true); }
}
void MtpExportService::consumeLine(const QByteArray &line) {
    if (line.isEmpty()) return;
    const auto document = QJsonDocument::fromJson(line);
    if (!document.isObject() || m_done) { m_error = QStringLiteral("设备导出工具返回了无效或重复的完成记录。"); return; }
    const auto object = document.object(); const auto type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("progress")) {
        emit taskProgress(object.value(QStringLiteral("message")).toString(), object.value(QStringLiteral("percent")).toInt(-1));
    } else if (type == QStringLiteral("created")) {
        m_location = object.value(QStringLiteral("location")).toString();
    } else if (type == QStringLiteral("error")) {
        m_error = object.value(QStringLiteral("message")).toString();
        if (!object.value(QStringLiteral("location")).toString().isEmpty()) m_location = object.value(QStringLiteral("location")).toString();
    } else if (type == QStringLiteral("cancelled")) {
        if (!object.value(QStringLiteral("location")).toString().isEmpty()) m_location = object.value(QStringLiteral("location")).toString();
        if (!m_cancelled && !m_timedOut) m_error = QStringLiteral("设备导出工具意外取消了任务。");
    } else if (type == QStringLiteral("destinations") && m_operation == Operation::List) {
        if (!object.value(QStringLiteral("destinations")).isArray()) { m_error = QStringLiteral("设备目标列表不完整。"); return; }
        QSet<QString> unique;
        for (const auto &value : object.value(QStringLiteral("destinations")).toArray()) {
            const auto item = value.toObject(); MtpExportDestination entry;
            entry.deviceId = item.value(QStringLiteral("deviceId")).toString(); entry.storageId = item.value(QStringLiteral("storageId")).toString();
            entry.rootId = item.value(QStringLiteral("rootId")).toString(); entry.gameId = item.value(QStringLiteral("gameId")).toString();
            entry.deviceName = item.value(QStringLiteral("deviceName")).toString(); entry.storageName = item.value(QStringLiteral("storageName")).toString();
            entry.gameName = item.value(QStringLiteral("gameName")).toString();
            const QString key = entry.deviceId + QChar(0) + entry.storageId + QChar(0) + entry.rootId;
            if (!validDestination(entry) || unique.contains(key)) { m_error = QStringLiteral("设备目标列表定位无效或重复。"); return; }
            unique.insert(key); m_destinations.append(entry);
        }
        m_done = true;
    } else if (type == QStringLiteral("uploaded") && m_operation == Operation::Upload) {
        if (QDir::cleanPath(object.value(QStringLiteral("localFolder")).toString()).compare(m_folder, Qt::CaseInsensitive) != 0
                || object.value(QStringLiteral("location")).toString().isEmpty()
                || !object.value(QStringLiteral("verified")).toBool(false)) { m_error = QStringLiteral("设备导出完成记录缺少文件回读校验或本地目录不匹配。"); return; }
        m_location = object.value(QStringLiteral("location")).toString(); m_done = true;
    } else m_error = QStringLiteral("设备导出工具返回未知或不匹配的记录。");
}
QString MtpExportService::residualMessage() const {
    return m_location.isEmpty() ? QString() : QStringLiteral("\n设备中可能保留本次未完成的目录：%1。完整电脑副本仍在 %2；请手动检查残留目录。").arg(m_location, m_folder);
}
void MtpExportService::finish(int exitCode, QProcess::ExitStatus status) {
    if (!isBusy()) return;
    readOutput(); if (!m_stdout.trimmed().isEmpty()) consumeLine(m_stdout.trimmed()); m_stdout.clear();
    m_timeout.stop(); m_cancelDeadline.stop(); const auto operation = m_operation; m_operation = Operation::None;
    if (m_cancelled) {
        if (!m_location.isEmpty()) emit errorOccurred(QStringLiteral("已取消设备导出。") + residualMessage());
        emit cancelled(); return;
    }
    if (m_timedOut || status != QProcess::NormalExit || exitCode != 0 || !m_error.isEmpty() || !m_done) {
        const QString message = m_timedOut ? QStringLiteral("设备导出超时，请检查头显连接。") : m_error.isEmpty()
            ? QStringLiteral("设备导出未完成或校验失败（工具返回 %1），未报告成功。").arg(exitCode) : m_error;
        emit errorOccurred(message + residualMessage()); return;
    }
    emit taskProgress(QStringLiteral("设备校验完成"), 100);
    if (operation == Operation::List) emit destinationsListed(m_destinations); else emit songUploaded(m_folder, m_location);
}
