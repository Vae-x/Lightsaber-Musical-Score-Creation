#include "ApiModelClient.h"
#include "AiDiagnostics.h"
#ifdef Q_OS_WIN
#include "WinHttpModelTransport.h"
#endif

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace lmsc {
namespace {

constexpr qint64 maximumResponseSize = 4 * 1024 * 1024;

QString validateAddress(const QUrl &url) {
    if (!url.isValid() || url.host().isEmpty()
            || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")))
        return QStringLiteral("请填写完整的 HTTPS API 地址。");
    if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return QStringLiteral("API 地址不能包含账户密码、查询参数或片段。");
    if (url.scheme() == QStringLiteral("http")) {
        const QHostAddress address(url.host());
        if (url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) != 0
                && !address.isLoopback())
            return QStringLiteral("远程 API 必须使用 HTTPS；HTTP 仅用于本机服务。");
    }
    return {};
}

QString httpFailure(int status) {
    switch (status) {
    case 401: return QStringLiteral("API Key 无效或已过期（HTTP 401），请检查密钥与服务地址。");
    case 403: return QStringLiteral("没有读取模型列表的权限（HTTP 403），请检查账户权限。");
    case 404: return QStringLiteral("服务未提供 /models 接口（HTTP 404），请检查地址或手动填写模型名称。");
    case 429: return QStringLiteral("请求过于频繁或账户额度不足（HTTP 429），请稍后重试。");
    default: return QStringLiteral("获取模型失败（HTTP %1），请检查服务状态。 ").arg(status).trimmed();
    }
}

} // namespace

ApiModelClient::ApiModelClient(QObject *parent)
    : QObject(parent), m_timeout(new QTimer(this)) {
    connect(this,&ApiModelClient::requestFailed,this,[this](const QString &) {
        m_diagnostic["elapsedMs"]=m_clock.isValid() ? double(m_clock.elapsed()) : 0;
        DiagnosticLog::instance().record("models.failed",m_diagnostic);
    });
    connect(this,&ApiModelClient::modelsReady,this,[this](const QStringList &) {
        m_diagnostic["elapsedMs"]=double(m_clock.elapsed());
        m_diagnostic.remove("category");
        DiagnosticLog::instance().record("models.completed",m_diagnostic);
    });
#ifdef Q_OS_WIN
    m_native = new WinHttpModelTransport(this);
    connect(m_native,&WinHttpModelTransport::networkDetails,this,[this](const QString &stage,int code) {
        if (!m_busy) return;
        m_diagnostic["stage"]=stage; m_diagnostic["nativeCode"]=code;
    });
    connect(m_native, &WinHttpModelTransport::replyReady, this,
            [this](int status, const QByteArray &contents, const QString &error) {
        if (!m_busy) return;
        m_busy = false;
        m_timeout->stop();
        completeReply(status, contents, error);
    });
#else
    m_network = new QNetworkAccessManager(this);
#endif
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (!m_busy) return;
        m_diagnostic["category"]="timeout";
        cancel();
        emit requestFailed(QStringLiteral("获取模型超时，请检查网络或服务地址后重试。"));
    });
}

bool ApiModelClient::isBusy() const {
    return m_busy;
}

void ApiModelClient::setProxyConfig(const NetworkProxyConfig &proxy) {
    if (m_proxy.mode == proxy.mode && m_proxy.host == proxy.host && m_proxy.port == proxy.port) return;
    cancel();
    m_proxy = proxy;
}

void ApiModelClient::cancel() {
    ++m_generation;
    m_busy = false;
    m_timeout->stop();
#ifdef Q_OS_WIN
    m_native->cancel();
#endif
    if (!m_reply) return;
    QNetworkReply *reply = m_reply.data();
    m_reply.clear();
    reply->abort();
    reply->deleteLater();
}

void ApiModelClient::fetchModels(const QString &baseUrl, const QString &apiKey,
                                const QString &providerId, int timeoutMs) {
    cancel();
    m_clock.start();
    DiagnosticLog::instance().addSecret(apiKey.trimmed());
    m_diagnostic={{"requestId",QStringLiteral("models-")+QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"provider",providerId},{"proxyMode",m_proxy.mode},{"stage","prepare"},{"category","parameters"},{"timeoutMs",timeoutMs}};
    DiagnosticLog::instance().record("models.started",m_diagnostic);
    const QString proxyError = AppSettings::validateProxy(m_proxy);
    if (!proxyError.isEmpty()) {
        emit requestFailed(proxyError);
        return;
    }
    QUrl url(baseUrl.trimmed(), QUrl::StrictMode);
    const QString addressError = validateAddress(url);
    if (!addressError.isEmpty()) {
        emit requestFailed(addressError);
        return;
    }
    const QString key = apiKey.trimmed();
    if (key.isEmpty()) {
        emit requestFailed(QStringLiteral("请先填写 API Key。"));
        return;
    }
    for (const auto &character : apiKey) {
        if (character.unicode() < 32 || character.unicode() == 127) {
            emit requestFailed(QStringLiteral("API Key 包含无效的控制字符，请重新粘贴。"));
            return;
        }
    }
#ifndef Q_OS_WIN
    if (url.scheme() == QStringLiteral("https") && !QSslSocket::supportsSsl()) {
        emit requestFailed(QStringLiteral("当前程序缺少匹配的 HTTPS/TLS 运行库，请使用包含网络运行库的完整程序包。"));
        return;
    }
#endif
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (!path.endsWith(QStringLiteral("/models"))) path += QStringLiteral("/models");
    url.setPath(path);
    m_diagnostic["host"]=url.host(); m_diagnostic["stage"]="http.wait";
    m_diagnostic["category"]="protocol";
    m_busy = true;
    m_timeout->start(qMax(1, timeoutMs));
#ifdef Q_OS_WIN
    m_native->fetch(url, key, providerId, timeoutMs, m_proxy);
#else
    // Local gateways stay on this computer in either proxy mode.
    const bool loopback = url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0
            || QHostAddress(url.host()).isLoopback();
    if (loopback || m_proxy.mode == "direct") {
        m_network->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    } else if (m_proxy.mode == QStringLiteral("manual")) {
        m_network->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, m_proxy.host.trimmed(),
                                         static_cast<quint16>(m_proxy.port)));
    } else {
        const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
        m_network->setProxy(proxies.isEmpty() ? QNetworkProxy(QNetworkProxy::NoProxy) : proxies.first());
    }
    const quint64 generation = m_generation;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    // MiMo's official OpenAI-compatible endpoint requires this additional header.
    if (providerId == QStringLiteral("mimo")) request.setRawHeader("api-key", key.toUtf8());
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    reply->setReadBufferSize(maximumResponseSize + 1);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, generation] {
        if (generation != m_generation || reply != m_reply) return;
        if (reply->bytesAvailable() > maximumResponseSize) {
            cancel();
            emit requestFailed(QStringLiteral("模型列表响应过大，已停止读取。"));
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        if (generation != m_generation || reply != m_reply) {
            reply->deleteLater();
            return;
        }
        m_timeout->stop();
        m_busy = false;
        m_reply.clear();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        const QByteArray contents = reply->read(maximumResponseSize + 1);
        reply->deleteLater();
        QString message;
        if (networkError != QNetworkReply::NoError) {
            message = networkError == QNetworkReply::SslHandshakeFailedError
                    ? QStringLiteral("TLS 安全连接失败，请检查系统时间、证书与服务地址。")
                    : QStringLiteral("无法连接模型服务，请检查网络、代理与服务地址（网络错误 %1）。")
                              .arg(static_cast<int>(networkError));
        }
        completeReply(status, contents, message);
    });
#endif
}

void ApiModelClient::completeReply(int status, const QByteArray &contents, const QString &error) {
    m_diagnostic["httpStatus"]=status;
    m_diagnostic["category"]=status>=400 ? aiHttpCategory(status) : !error.isEmpty()
        ? error.contains(QStringLiteral("TLS")) ? QString("tls") : error.contains(QStringLiteral("超时")) ? QString("timeout") : QString("network")
        : QString("protocol");
    if (status >= 300 && status < 400) {
        emit requestFailed(QStringLiteral("API 地址发生重定向，已停止请求以保护密钥；请填写最终服务地址。"));
        return;
    }
    if (status >= 400) {
        // Do not display untrusted server bodies, which may echo credentials.
        emit requestFailed(httpFailure(status));
        return;
    }
    if (!error.isEmpty()) {
        emit requestFailed(error);
        return;
    }
    if (contents.size() > maximumResponseSize) {
        emit requestFailed(QStringLiteral("模型列表响应过大，已停止读取。"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !document.object().value(QStringLiteral("data")).isArray()) {
        emit requestFailed(QStringLiteral("服务返回的模型列表格式无效，需要 OpenAI 兼容的 data[].id。"));
        return;
    }
    QStringList models;
    for (const auto &item : document.object().value(QStringLiteral("data")).toArray()) {
        const auto id = item.toObject().value(QStringLiteral("id"));
        if (!id.isString()) continue;
        const QString model = id.toString().trimmed();
        if (!model.isEmpty()) models.append(model);
    }
    models.removeDuplicates();
    models.sort(Qt::CaseInsensitive);
    if (models.isEmpty()) {
        emit requestFailed(QStringLiteral("服务没有返回可用模型，可以手动填写模型名称。"));
        return;
    }
    emit modelsReady(models);
}

} // namespace lmsc
