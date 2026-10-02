#pragma once

#include "AppSettings.h"

#include <QObject>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace lmsc {
#ifdef Q_OS_WIN
class WinHttpModelTransport;
#endif

// Fetches OpenAI-compatible GET /models asynchronously. Never follows redirects
// with credentials; TLS certificate verification stays enabled.
class ApiModelClient : public QObject {
    Q_OBJECT
public:
    explicit ApiModelClient(QObject *parent = nullptr);
    void setProxyConfig(const NetworkProxyConfig &proxy);
    void fetchModels(const QString &baseUrl, const QString &apiKey,
                     const QString &providerId, int timeoutMs = 20000);
    void cancel();
    bool isBusy() const;

signals:
    void modelsReady(const QStringList &models);
    void requestFailed(const QString &message);

private:
    void completeReply(int status, const QByteArray &contents, const QString &error);
#ifdef Q_OS_WIN
    WinHttpModelTransport *m_native = nullptr;
#endif
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_timeout = nullptr;
    quint64 m_generation = 0;
    bool m_busy = false;
    NetworkProxyConfig m_proxy;
    QElapsedTimer m_clock;
    QJsonObject m_diagnostic;
};

} // namespace lmsc
