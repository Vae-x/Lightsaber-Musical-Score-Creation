#include "MtpDeleteService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
bool remoteName(const QString &name) {
    if (name.trimmed().isEmpty() || name == "." || name == "..") return false;
    for (const auto character : name)
        if (character == '/' || character == '\\' || character.category() == QChar::Other_Control) return false;
    return true;
}
bool deleteLocator(const MtpSongEntry &entry, QJsonObject *locator) {
    if (!entry.isSong) return false;
    const auto document = QJsonDocument::fromJson(entry.locator.toUtf8());
    if (!document.isObject()) return false;
    *locator = document.object();
    if (!locator->value("device").isString() || !remoteName(locator->value("device").toString())
            || !locator->value("segments").isArray()) return false;
    const auto segments = locator->value("segments").toArray();
    for (const auto &segment : segments) if (!segment.isString() || !remoteName(segment.toString())) return false;
    const QStringList roots = entry.gameId == "oasis" ? QStringList{"SoulTopia", "BeatNote", "Custom"}
        : entry.gameId == "lightband" ? QStringList{"Android", "data", "com.StarRiverVR.LightBand", "files", "CustomMusic"} : QStringList{};
    if (roots.isEmpty() || segments.size() < roots.size() + 2 || segments.size() > roots.size() + 21) return false;
    for (int index = 0; index < roots.size(); ++index)
        if (segments.at(index + 1).toString() != roots.at(index)) return false;
    return entry.name == segments.last().toString() && entry.deviceName == locator->value("device").toString()
        && (entry.storageName.isEmpty() || entry.storageName == segments.first().toString());
}
}

MtpDeleteService::MtpDeleteService(QObject *parent) : QObject(parent) {
#ifdef Q_OS_WIN
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW; arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        arguments->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
    m_timeout.setSingleShot(true); m_stopDeadline.setSingleShot(true);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &MtpDeleteService::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { m_process.readAllStandardError(); });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MtpDeleteService::finish);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_busy) return;
        m_timeout.stop(); m_stopDeadline.stop(); m_busy = m_prepared = false; m_planToken.clear();
        emit errorOccurred(QStringLiteral("无法启动头显歌曲删除工具：%1").arg(m_process.errorString()));
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] { requestStop(true); });
    connect(&m_stopDeadline, &QTimer::timeout, this, [this] { if (m_process.state() != QProcess::NotRunning) m_process.kill(); });
}
MtpDeleteService::~MtpDeleteService() {
    m_process.disconnect(this); m_timeout.stop(); m_stopDeadline.stop();
    if (m_process.state() != QProcess::NotRunning) {
        QFile marker(m_cancelFile); if (marker.open(QIODevice::WriteOnly)) marker.write("cancel");
        if (!m_process.waitForFinished(3000)) { m_process.kill(); m_process.waitForFinished(3000); }
    }
}
void MtpDeleteService::setHelperPath(const QString &path) { m_helper = path; }
QString MtpDeleteService::helperPath() const {
    if (!m_helper.isEmpty()) return m_helper;
    QDir directory(QCoreApplication::applicationDirPath());
    for (int level = 0; level < 8; ++level) {
        for (const auto &relative : {QStringLiteral("tools/mtp/WpdTransfer.exe"), QStringLiteral("build/tools/mtp/WpdTransfer.exe")}) {
            const QString path = directory.filePath(relative); if (QFileInfo::exists(path)) return path;
        }
        if (!directory.cdUp()) break;
    }
    return {};
}
bool MtpDeleteService::isBusy() const { return m_busy; }
void MtpDeleteService::prepareSong(const MtpSongEntry &entry) {
    if (m_busy) { emit errorOccurred(QStringLiteral("正在处理头显歌曲删除，请等待操作结束。")); return; }
    m_prepared = false; m_planToken.clear();
    QJsonObject locator;
    if (!deleteLocator(entry, &locator)) {
        emit errorOccurred(QStringLiteral("只能删除所选游戏歌曲根目录内的单首歌曲，请重新刷新并选择歌曲节点。")); return;
    }
#ifndef Q_OS_WIN
    emit errorOccurred(QStringLiteral("头显歌曲删除目前仅支持 Windows。")); return;
#endif
    const QString helper = helperPath();
    if (!QFileInfo(helper).isFile()) { emit errorOccurred(QStringLiteral("缺少 tools/mtp/WpdTransfer.exe，请重新解压完整便携包。")); return; }
    m_session.reset(new QTemporaryDir(QDir::tempPath() + QStringLiteral("/lmsc-wpd-delete-XXXXXX")));
    if (!m_session->isValid()) { emit errorOccurred(QStringLiteral("无法创建头显歌曲删除任务目录。")); return; }
    m_entry = entry; m_pendingHelper = helper; m_locator = locator;
    QStringList segments;
    for (const auto &segment : locator.value("segments").toArray()) segments.append(segment.toString());
    m_entry.location = segments.join(QLatin1Char('\\'));
    m_entry.gameName = entry.gameId == "oasis" ? QStringLiteral("星穹绿洲") : QStringLiteral("光之乐团");
    m_entry.storageName = segments.first();
    m_location = locator.value("device").toString() + QLatin1Char('\\') + m_entry.location;
    m_cancelFile = m_session->filePath(QStringLiteral("cancel"));
    m_planFile = m_session->filePath(QStringLiteral("plan.json"));
    m_preparing = true; launch(QStringLiteral("delete-prepare"));
}
void MtpDeleteService::deletePrepared() {
    if (m_busy) { emit errorOccurred(QStringLiteral("正在处理头显歌曲删除，请等待操作结束。")); return; }
    if (!m_prepared || m_planToken.isEmpty() || !m_session || !QFileInfo::exists(m_planFile)) {
        emit errorOccurred(QStringLiteral("请先重新核对所选歌曲，再确认删除。")); return;
    }
    m_prepared = false; m_preparing = false; launch(QStringLiteral("delete"));
}
void MtpDeleteService::launch(const QString &mode) {
    const QString jobPath = m_session->filePath(QStringLiteral("job.json"));
    const QJsonObject job{{"protocol", 1}, {"mode", mode}, {"locator", m_locator}, {"name", m_entry.name},
        {"gameId", m_entry.gameId}, {"cancelFile", m_cancelFile}, {"planFile", m_planFile}, {"planToken", m_planToken}};
    QFile file(jobPath); const auto data = QJsonDocument(job).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) { emit errorOccurred(QStringLiteral("无法写入头显歌曲删除任务。")); return; }
    file.close(); m_error.clear(); m_stdout.clear(); m_done = m_startedDeletion = m_cancelled = m_timedOut = false;
    m_busy = true; m_timeout.start(900000);
    const QString helper = m_pendingHelper;
    if (QFileInfo(helper).suffix().compare(QStringLiteral("ps1"), Qt::CaseInsensitive) == 0) {
        const QString windows = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
        QString powershell = windows + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
#if defined(Q_OS_WIN) && !defined(_WIN64)
        const QString native = windows + QStringLiteral("/Sysnative/WindowsPowerShell/v1.0/powershell.exe");
        if (QFileInfo::exists(native)) powershell = native;
#endif
        m_process.setProgram(powershell); m_process.setArguments({"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
            QDir::toNativeSeparators(helper), "-JobPath", QDir::toNativeSeparators(jobPath)});
    } else { m_process.setProgram(helper); m_process.setArguments({"--job", QDir::toNativeSeparators(jobPath)}); }
    emit taskProgress(m_preparing ? QStringLiteral("正在核对所选头显歌曲，尚未删除") : QStringLiteral("正在重新核对已确认的歌曲，随后删除"), -1);
    if (m_busy && !m_cancelled && !m_timedOut) m_process.start();
}
void MtpDeleteService::cancel() { m_prepared = false; m_planToken.clear(); if (m_busy) requestStop(false); }
void MtpDeleteService::requestStop(bool timedOut) {
    if (m_cancelled || m_timedOut) return;
    m_timeout.stop(); m_timedOut = timedOut; m_cancelled = !timedOut; m_prepared = false; m_planToken.clear();
    QFile marker(m_cancelFile); if (marker.open(QIODevice::WriteOnly)) marker.write("cancel");
    if (m_process.state() == QProcess::NotRunning) {
        m_busy = false;
        if (timedOut) emit errorOccurred(QStringLiteral("头显歌曲删除超时。") + interruptionMessage()); else emit cancelled();
        return;
    }
    emit taskProgress(m_preparing ? QStringLiteral("正在取消歌曲核对，尚未删除") : QStringLiteral("正在停止后续删除；已删除的文件无法撤销"), -1); m_stopDeadline.start(8000);
}
void MtpDeleteService::readOutput() {
    m_stdout += m_process.readAllStandardOutput();
    int newline;
    while ((newline = m_stdout.indexOf('\n')) >= 0) {
        const auto line = m_stdout.left(newline).trimmed(); m_stdout.remove(0, newline + 1); consumeLine(line);
    }
    if (m_stdout.size() > 2 * 1024 * 1024) { m_error = QStringLiteral("头显删除工具返回数据超过限制。"); requestStop(true); }
}
void MtpDeleteService::consumeLine(const QByteArray &line) {
    if (line.isEmpty()) return;
    const auto document = QJsonDocument::fromJson(line);
    if (!document.isObject() || m_done) { m_error = QStringLiteral("头显删除工具返回无效或重复的记录。"); return; }
    const auto record = document.object(); const QString type = record.value("type").toString();
    if (type == "progress") emit taskProgress(record.value("message").toString(), record.value("percent").toInt(-1));
    else if (type == "prepared" && m_preparing) {
        const QString token = record.value("planToken").toString();
        const double files = record.value("files").toDouble(-1);
        const double bytes = record.value("bytes").toDouble(-1);
        if (record.value("locator").toObject() != m_locator || !QFileInfo::exists(m_planFile)
                || !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(token).hasMatch()
                || !std::isfinite(files) || std::floor(files) != files || files < 1 || files > 10000
                || !std::isfinite(bytes) || std::floor(bytes) != bytes || bytes < 0 || bytes > 8LL * 1024 * 1024 * 1024) {
            m_error = QStringLiteral("头显歌曲预备清单缺少有效的对象核对信息。"); return;
        }
        m_files = int(files); m_bytes = qint64(bytes); m_planToken = token; m_done = true;
    } else if (type == "deleting" && !m_preparing) {
        m_startedDeletion = true;
        if (record.value("locator").toObject() != m_locator) { m_error = QStringLiteral("头显删除对象与所选歌曲不一致。"); requestStop(false); return; }
    } else if (type == "deleted" && !m_preparing) {
        if (record.value("locator").toObject() != m_locator || !record.value("verified").toBool(false) || !m_startedDeletion) {
            m_error = QStringLiteral("头显删除完成记录缺少目标核对或删除结果校验。"); return;
        }
        m_done = true;
    } else if (type == "error") {
        m_error = record.value("message").toString(); m_startedDeletion |= record.value("deletionStarted").toBool(false);
    } else if (type == "cancelled") {
        m_startedDeletion |= record.value("deletionStarted").toBool(false);
        if (!m_cancelled && !m_timedOut) m_error = QStringLiteral("头显删除工具意外停止了任务。");
    } else m_error = QStringLiteral("头显删除工具返回未知记录。");
}
QString MtpDeleteService::interruptionMessage() const {
    return m_startedDeletion ? QStringLiteral("\n所选歌曲可能已部分删除：%1。已删除内容无法撤销，请刷新歌曲列表并检查头显；不会继续删除其他目录。").arg(m_location)
        : QStringLiteral("\n尚未确认删除完成，请刷新歌曲列表核对所选歌曲。");
}
void MtpDeleteService::finish(int exitCode, QProcess::ExitStatus status) {
    if (!m_busy) return;
    readOutput(); if (!m_stdout.trimmed().isEmpty()) consumeLine(m_stdout.trimmed()); m_stdout.clear();
    m_timeout.stop(); m_stopDeadline.stop(); m_busy = false;
    // 删除完成且核对无误后，即使同时收到停止请求，也如实报告删除结果。
    if (m_done && exitCode == 0 && status == QProcess::NormalExit && m_error.isEmpty()) {
        if (m_preparing && !m_cancelled && !m_timedOut) {
            m_prepared = true; emit taskProgress(QStringLiteral("歌曲核对完成，等待确认"), 100);
            emit deletionPrepared(m_entry, m_files, m_bytes); return;
        }
        if (m_preparing) { m_prepared = false; m_planToken.clear(); emit cancelled(); return; }
        emit taskProgress(QStringLiteral("所选头显歌曲已删除并核对"), 100); emit songDeleted(); return;
    }
    if (m_cancelled) {
        m_prepared = false; m_planToken.clear();
        if (m_startedDeletion || !m_error.isEmpty()) {
            emit errorOccurred((m_error.isEmpty() ? QStringLiteral("已停止后续删除。") : m_error) + interruptionMessage()); return;
        }
        emit cancelled(); return;
    }
    m_prepared = false; m_planToken.clear();
    const QString message = m_timedOut ? QStringLiteral("头显歌曲删除超时，请检查设备连接。") : m_error.isEmpty()
        ? QStringLiteral("头显歌曲删除未完成或结果未核对（工具返回 %1）。").arg(exitCode) : m_error;
    emit errorOccurred(message + interruptionMessage());
}
