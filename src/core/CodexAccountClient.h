#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace lmsc {

// 账号和模型设置使用官方 app-server；不读取令牌文件，也不创建 thread/turn。
class CodexAccountClient : public QObject {
    Q_OBJECT
public:
    explicit CodexAccountClient(QObject *parent = nullptr);
    ~CodexAccountClient() override;

    void setExecutablePath(const QString &path);
    QString executablePath() const;
    static QString detectedExecutable();
    void checkAccount();
    void beginLogin();
    void cancelLogin();
    void fetchModels();
    void stop();

signals:
    void accountStatus(const QString &message, bool loggedIn);
    void authorizationRequired(const QUrl &url);
    void loginFinished(bool success, const QString &message);
    void modelsReady(const QStringList &models);
    void requestFailed(const QString &message);

private:
    enum class Operation { Account, Login, Models };
    struct PendingRequest { QString method; qint64 deadline = 0; };
    void enqueue(Operation operation);
    void startServer();
    void dispatchOperations();
    int sendRequest(const QString &method, const QJsonObject &parameters = {});
    void sendMessage(const QJsonObject &message);
    void readOutput();
    void handleMessage(const QJsonObject &message);
    void handleResult(const QString &method, const QJsonObject &result);
    void requestModelsPage(const QString &cursor = {});
    void fail(const QString &message);
    void resetState();
    bool hasRequest(const QString &method) const;
    bool commandForPath(const QString &path, QString *program, QStringList *arguments) const;

    QProcess m_process;
    QTimer m_watchdog;
    QTimer m_loginTimeout;
    QElapsedTimer m_clock;
    QHash<int, PendingRequest> m_pending;
    QQueue<Operation> m_operations;
    QByteArray m_output;
    QString m_executable;
    QString m_loginId;
    QStringList m_models;
    QSet<QString> m_modelCursors;
    int m_nextId = 1;
    qint64 m_startDeadline = 0;
    bool m_initialized = false;
    bool m_stopping = false;
    bool m_loginRequested = false;
    bool m_cancelLoginPending = false;
    bool m_fetchingModels = false;
};

} // namespace lmsc
