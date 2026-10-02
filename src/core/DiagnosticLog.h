#pragma once
#include <QObject>
#include <QJsonObject>
#include <QMutex>
#include <QStringList>

namespace lmsc {
// Only explicitly allowed diagnostic fields are written. Never a raw output sink.
class DiagnosticLog : public QObject {
    Q_OBJECT
public:
    explicit DiagnosticLog(const QString &directory = {}, qint64 maximumBytes = 5 * 1024 * 1024,
                           int maximumFiles = 5, QObject *parent = nullptr);
    static DiagnosticLog &instance();
    void setEnabled(bool enabled);
    bool isEnabled() const;
    void addSecret(const QString &secret);
    void record(const QString &event, const QJsonObject &fields = {});
    QString directory() const;
    QString lastError() const;
    QByteArray read(const QString &jobId = {}) const;
    bool exportTo(const QString &path, const QString &jobId, QString *error) const;
signals:
    void updated();
private:
    QString path(int index) const;
    QJsonObject sanitize(const QString &event, const QJsonObject &fields) const;
    mutable QMutex m_mutex;
    QString m_directory, m_error;
    QStringList m_secrets;
    qint64 m_maximumBytes;
    int m_maximumFiles;
    bool m_enabled = true;
};
}
