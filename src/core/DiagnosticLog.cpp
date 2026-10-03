#include "DiagnosticLog.h"
#include "AppInfo.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSet>

namespace lmsc {
DiagnosticLog::DiagnosticLog(const QString &directory, qint64 maximumBytes, int maximumFiles, QObject *parent)
    : QObject(parent), m_directory(directory.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("logs")
        : QDir(directory).absolutePath()), m_maximumBytes(qMax<qint64>(512, maximumBytes)),
      m_maximumFiles(qBound(1, maximumFiles, 5)) {}
DiagnosticLog &DiagnosticLog::instance() { static DiagnosticLog log; return log; }
void DiagnosticLog::setEnabled(bool enabled) { QMutexLocker lock(&m_mutex); m_enabled = enabled; }
bool DiagnosticLog::isEnabled() const { QMutexLocker lock(&m_mutex); return m_enabled; }
void DiagnosticLog::addSecret(const QString &secret) {
    if (secret.isEmpty()) return;
    QMutexLocker lock(&m_mutex);
    if (!m_secrets.contains(secret)) m_secrets.append(secret);
}
QString DiagnosticLog::path(int index) const { return QDir(m_directory).filePath(QString("diagnostic-%1.jsonl").arg(index)); }
QString DiagnosticLog::directory() const { QMutexLocker lock(&m_mutex); return m_directory; }
QString DiagnosticLog::lastError() const { QMutexLocker lock(&m_mutex); return m_error; }
QJsonObject DiagnosticLog::sanitize(const QString &event, const QJsonObject &fields) const {
    static const QSet<QString> strings{"jobId", "requestId", "connection", "provider", "model", "proxyMode",
        "host", "stage", "category", "reason", "method", "cliVersion", "build", "code", "decision"};
    static const QSet<QString> numbers{"port", "elapsedMs", "timeoutMs", "httpStatus", "rpcCode", "nativeCode",
        "exitCode", "segment", "segments", "percent", "objects", "repairs", "inputTokens", "outputTokens", "maxOutputTokens",
        "reasoningTokens", "finalTextBytes", "recoveryAttempt", "automaticRecoveries", "delayMs", "previousMaxOutputTokens",
        "referenceSegment", "repeatConfidence", "referenceNotes", "currentNotes", "matchedActions", "actionDifference",
        "rhythmCoverage", "positionDifference", "countDifference", "targetVariation"};
    QJsonObject row{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {"version", AppInfo::version()},
                    {"build", QString::fromLatin1(__DATE__).replace(' ', '-')+'/'+QString::fromLatin1(__TIME__)}};
    static const QRegularExpression identifier("^[A-Za-z0-9_.:/\\-]{1,128}$");
    const auto clean = [this](QString value) {
        for (const auto &secret : m_secrets) value.replace(secret, "redacted");
        if (!identifier.match(value).hasMatch() || value.contains("sk-", Qt::CaseInsensitive)
            || value.contains("Bearer", Qt::CaseInsensitive) || value.contains("://")
            || QRegularExpression("^[A-Za-z]:/").match(value).hasMatch()
            || (value.count('.') >= 2 && value.size() > 100))
            return QStringLiteral("redacted");
        return value;
    };
    row.insert("event", clean(event));
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (strings.contains(it.key()) && it.value().isString()) row.insert(it.key(), clean(it.value().toString()));
        else if (numbers.contains(it.key()) && it.value().isDouble()) row.insert(it.key(), it.value());
        else if (it.key() == "retryable" && it.value().isBool()) row.insert(it.key(), it.value());
    }
    return row;
}
void DiagnosticLog::record(const QString &event, const QJsonObject &fields) {
    {
        QMutexLocker lock(&m_mutex);
        if (!m_enabled) return;
        if (!QDir().mkpath(m_directory)) { m_error = tr("无法创建诊断日志目录。"); return; }
        for (int i = 0; i < m_maximumFiles; ++i) {
            const QFileInfo info(path(i));
            if (info.isFile() && info.lastModified().toUTC() < QDateTime::currentDateTimeUtc().addDays(-7)) QFile::remove(path(i));
        }
        const QByteArray line = QJsonDocument(sanitize(event, fields)).toJson(QJsonDocument::Compact) + '\n';
        if (QFileInfo(path(0)).size() + line.size() > m_maximumBytes) {
            QFile::remove(path(m_maximumFiles - 1));
            for (int i = m_maximumFiles - 2; i >= 0; --i)
                if (QFile::exists(path(i)) && !QFile::rename(path(i), path(i + 1))) {
                    m_error = tr("诊断日志轮转失败。"); return;
                }
        }
        QFile file(path(0));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append) || file.write(line) != line.size()) {
            m_error = tr("诊断日志无法写入，请检查目录权限。"); return;
        }
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        m_error.clear();
    }
    emit updated();
}
QByteArray DiagnosticLog::read(const QString &jobId) const {
    QMutexLocker lock(&m_mutex);
    QByteArray contents;
    for (int i = m_maximumFiles - 1; i >= 0; --i) {
        QFile file(path(i));
        if (QFileInfo(file).lastModified().toUTC() < QDateTime::currentDateTimeUtc().addDays(-7)) continue;
        if (!file.open(QIODevice::ReadOnly)) continue;
        if (file.size()>m_maximumBytes) { file.seek(file.size()-m_maximumBytes); file.readLine(8192); }
        while (!file.atEnd()) {
            const QByteArray line = file.readLine(8192);
            const auto row = QJsonDocument::fromJson(line).object();
            if (row.isEmpty() || (!jobId.isEmpty() && row.value("jobId").toString() != jobId)) continue;
            // Re-sanitize on export/view as an extra barrier against externally edited files.
            QJsonObject safe = sanitize(row.value("event").toString(), row);
            if (QDateTime::fromString(row.value("time").toString(), Qt::ISODateWithMs).isValid()) safe["time"] = row["time"];
            contents += QJsonDocument(safe).toJson(QJsonDocument::Compact) + '\n';
        }
    }
    return contents;
}
bool DiagnosticLog::exportTo(const QString &path, const QString &jobId, QString *error) const {
    if (error) error->clear();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error) *error = tr("无法创建导出文件，请选择一个尚不存在的新文件名。"); return false;
    }
    const auto contents = read(jobId);
    if (file.write(contents) != contents.size()) {
        if (error) *error = tr("日志导出失败。"); return false;
    }
    return true;
}
}
