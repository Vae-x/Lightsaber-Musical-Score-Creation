#pragma once

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>
#include <QStringList>
#include <memory>
#include <vector>

// Windows 便携设备没有磁盘路径；locator 仅用于重新定位 Shell 命名空间项目。
struct MtpSongEntry {
    QString name;
    QString deviceName;
    QString location;
    QString locator;
    QString gameName;
    QString gameId;
    QString storageName;
    QStringList categorySegments;
    // Empty game roots are also returned for browsing, but cannot be imported.
    bool isSong = true;
};
Q_DECLARE_METATYPE(MtpSongEntry)
Q_DECLARE_METATYPE(QVector<MtpSongEntry>)

class MtpImportService : public QObject {
    Q_OBJECT
public:
    explicit MtpImportService(QObject *parent = nullptr);
    ~MtpImportService() override;
    void setScriptPath(const QString &path);
    QString scriptPath() const;
    bool isBusy() const;

public slots:
    void listSongs();
    void importSong(const MtpSongEntry &entry);
    void cancel();

signals:
    void songsListed(const QVector<MtpSongEntry> &songs);
    // 临时副本属于此服务；服务销毁之前始终保留，设备原文件不会修改。
    void songImported(const QString &localFolder);
    void taskProgress(const QString &task, int percent);
    void errorOccurred(const QString &message);
    void cancelled();

private:
    enum class Operation { None, List, Import };
    void start(Operation operation, const QString &locator = QString());
    void readOutput();
    void consumeLine(const QByteArray &line);
    void finish(int exitCode, QProcess::ExitStatus status);
    void fail(const QString &message);
    QString powershellPath() const;
    QProcess m_process;
    QTimer m_timeout;
    QString m_script;
    QString m_target;
    QString m_error;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QVector<MtpSongEntry> m_songs;
    Operation m_operation = Operation::None;
    bool m_cancelled = false;
    bool m_done = false;
    bool m_timedOut = false;
    std::vector<std::unique_ptr<QTemporaryDir>> m_sessions;
};
