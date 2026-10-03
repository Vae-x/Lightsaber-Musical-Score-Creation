#pragma once

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QTimer>
#include <QVector>
#include <memory>
#include <atomic>

// storageId/rootId 使用 WPD 持久标识；辅助程序在每次连接中重新定位并核对父子链。
struct MtpExportDestination {
    QString deviceId, storageId, rootId;
    QString deviceName, storageName, gameId, gameName;
};
Q_DECLARE_METATYPE(MtpExportDestination)
Q_DECLARE_METATYPE(QVector<MtpExportDestination>)

class MtpExportService : public QObject {
    Q_OBJECT
public:
    explicit MtpExportService(QObject *parent = nullptr);
    ~MtpExportService() override;
    void setHelperPath(const QString &path);
    QString helperPath() const;
    bool isBusy() const;

public slots:
    void listDestinations();
    void uploadSong(const QString &localFolder, const MtpExportDestination &destination);
    void cancel();

signals:
    void destinationsListed(const QVector<MtpExportDestination> &destinations);
    void songUploaded(const QString &localFolder, const QString &deviceLocation);
    void taskProgress(const QString &task, int percent);
    void errorOccurred(const QString &message);
    void cancelled();

private:
    struct ManifestResult { QJsonArray files, references; QString error; };
    enum class Operation { None, List, Upload };
    void start(Operation operation, const QString &folder = QString(), const MtpExportDestination &destination = {});
    void launch(const QJsonArray &files = {}, const QJsonArray &references = {});
    void readOutput();
    void consumeLine(const QByteArray &line);
    void finish(int exitCode, QProcess::ExitStatus status);
    void requestStop(bool timeout);
    QString residualMessage() const;
    QProcess m_process;
    QTimer m_timeout, m_cancelDeadline;
    QFutureWatcher<ManifestResult> m_preparer;
    std::shared_ptr<std::atomic_bool> m_stopPreparation;
    std::unique_ptr<QTemporaryDir> m_session;
    QString m_helper, m_pendingHelper, m_folder, m_location, m_cancelFile, m_error;
    MtpExportDestination m_destination;
    QByteArray m_stdout;
    QVector<MtpExportDestination> m_destinations;
    Operation m_operation = Operation::None;
    bool m_cancelled = false, m_timedOut = false, m_done = false;
    bool m_preparing = false;
};
