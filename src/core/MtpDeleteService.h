#pragma once

#include "MtpImportService.h"

#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// 仅删除调用者逐首确认的歌曲目录。设备和游戏根目录由辅助程序再次校验。
class MtpDeleteService : public QObject {
    Q_OBJECT
public:
    explicit MtpDeleteService(QObject *parent = nullptr);
    ~MtpDeleteService() override;
    void setHelperPath(const QString &path);
    QString helperPath() const;
    bool isBusy() const;

public slots:
    void prepareSong(const MtpSongEntry &entry);
    void deletePrepared();
    void cancel();

signals:
    void deletionPrepared(const MtpSongEntry &entry, int files, qint64 bytes);
    void songDeleted();
    void taskProgress(const QString &task, int percent);
    void errorOccurred(const QString &message);
    void cancelled();

private:
    void launch(const QString &mode);
    void readOutput();
    void consumeLine(const QByteArray &line);
    void finish(int exitCode, QProcess::ExitStatus status);
    void requestStop(bool timedOut);
    QString interruptionMessage() const;
    QProcess m_process;
    QTimer m_timeout, m_stopDeadline;
    std::unique_ptr<QTemporaryDir> m_session;
    QString m_helper, m_pendingHelper, m_cancelFile, m_planFile, m_planToken, m_error, m_location;
    MtpSongEntry m_entry;
    QJsonObject m_locator;
    QByteArray m_stdout;
    bool m_busy = false, m_done = false, m_startedDeletion = false;
    bool m_cancelled = false, m_timedOut = false;
    bool m_preparing = false, m_prepared = false;
    int m_files = 0;
    qint64 m_bytes = 0;
};
