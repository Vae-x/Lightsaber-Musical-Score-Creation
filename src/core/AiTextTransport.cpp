#include "AiTextTransport.h"
#include "CodexAccountClient.h"
#include "CodexTextSession.h"
#ifdef Q_OS_WIN
#include "WinHttpModelTransport.h"
#endif

#include <QFileInfo>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>
#include <limits>

namespace lmsc {
namespace {
constexpr int maximumResponseSize = 4 * 1024 * 1024;

AiOutputPolicy policyFor(const AppPreferences &preferences) {
    if (preferences.aiConnection != "api") return {};
    const auto config = preferences.providers.value(preferences.providerId);
    const QUrl url(config.baseUrl.trimmed());
    const bool official = preferences.providerId == "deepseek" && url.scheme() == "https"
        && url.host().compare("api.deepseek.com", Qt::CaseInsensitive) == 0 && url.port(443) == 443
        && QStringList{"", "/", "/v1", "/v1/", "/chat/completions", "/v1/chat/completions"}.contains(url.path())
        && QStringList{"deepseek-flash", "deepseek-v4-pro"}.contains(config.model.trimmed());
    const int maximum = config.maxOutputTokens > 0 ? config.maxOutputTokens : official ? 131072 : 0;
    return {maximum > 0 ? qMin(official ? 65536 : 8192, maximum) : 0, maximum, official};
}

QString validateApi(const AiProviderConfig &config) {
    if (config.maxOutputTokens != 0 && (config.maxOutputTokens < 1024 || config.maxOutputTokens > 131072))
        return QStringLiteral("最大输出额度应为自动，或 1024–131072 token。");
    const QUrl url(config.baseUrl.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()
            || (url.scheme() != "https" && url.scheme() != "http"))
        return QStringLiteral("请在设置中填写完整的 HTTPS API 地址。 ").trimmed();
    if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return QStringLiteral("API 地址不能包含账户密码、查询参数或片段。 ").trimmed();
    if (url.scheme() == "http" && url.host().compare("localhost", Qt::CaseInsensitive) != 0
            && !QHostAddress(url.host()).isLoopback())
        return QStringLiteral("远程 API 必须使用 HTTPS；HTTP 仅用于本机服务。 ").trimmed();
    if (config.apiKey.trimmed().isEmpty()) return QStringLiteral("请先在设置中填写 API Key。 ").trimmed();
    for (const QChar character : config.apiKey)
        if (character.unicode() < 32 || character.unicode() == 127)
            return QStringLiteral("API Key 包含无效的控制字符，请重新粘贴。 ").trimmed();
    if (config.model.trimmed().isEmpty()) return QStringLiteral("请先在设置中选择或填写 AI 模型。 ").trimmed();
    return {};
}

QString validatePreferences(const AppPreferences &preferences) {
    const QString proxyError = AppSettings::validateProxy(preferences.networkProxy);
    if (!proxyError.isEmpty()) return proxyError;
    if (preferences.aiConnection == "api") return validateApi(preferences.providers.value(preferences.providerId));
    if (preferences.aiConnection != "codex") return QStringLiteral("请在设置中选择 AI 接入方式。 ").trimmed();
    if (preferences.codexModel.trimmed().isEmpty())
        return QStringLiteral("请先在设置中选择或填写 Codex 模型。 ").trimmed();
    const QString path = preferences.codexExecutable.trimmed().isEmpty()
            ? CodexAccountClient::detectedExecutable() : preferences.codexExecutable.trimmed();
    if (!QFileInfo(path).isFile()) return QStringLiteral("请先在设置中指定可用的 Codex CLI。 ").trimmed();
    return {};
}

bool sameConnection(const AppPreferences &a, const AppPreferences &b) {
    if (a.aiConnection != b.aiConnection || a.networkProxy.mode != b.networkProxy.mode
            || a.networkProxy.host != b.networkProxy.host || a.networkProxy.port != b.networkProxy.port
            || a.requestTimeoutMinutes != b.requestTimeoutMinutes) return false;
    if (a.aiConnection == "codex")
        return a.codexExecutable == b.codexExecutable && a.codexModel == b.codexModel;
    if (a.providerId != b.providerId) return false;
    const AiProviderConfig first = a.providers.value(a.providerId);
    const AiProviderConfig second = b.providers.value(b.providerId);
    return first.baseUrl == second.baseUrl && first.apiKey == second.apiKey && first.model == second.model
        && first.maxOutputTokens == second.maxOutputTokens;
}

QString httpError(int status) {
    switch (status) {
    case 401: return QStringLiteral("API Key 无效或已过期（HTTP 401）。");
    case 403: return QStringLiteral("账户没有使用此模型的权限（HTTP 403）。");
    case 404: return QStringLiteral("未找到生成接口或模型（HTTP 404），请检查 API 地址与模型名称。");
    case 429: return QStringLiteral("请求过于频繁或账户额度不足（HTTP 429），请稍后手动重试。");
    default: return QStringLiteral("AI 服务返回 HTTP %1，未应用任何结果。 ").arg(status).trimmed();
    }
}

int tokenCount(const QJsonValue &value) {
    if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > std::numeric_limits<int>::max()) return -1;
    const int count = value.toInt(-1);
    return value.toDouble() == count ? count : -1;
}
} // namespace

struct ConfiguredAiTextTransport::Impl {
    ConfiguredAiTextTransport *owner;
    AppPreferences preferences;
    AiTextRequest request;
    QTimer timeout;
    QTimer heartbeat;
    QPointer<CodexTextSession> codex;
    QPointer<QNetworkReply> reply;
    QNetworkAccessManager network;
#ifdef Q_OS_WIN
    WinHttpModelTransport native;
#endif
    quint64 generation = 0;
    QByteArray response;
    bool active = false;
    QElapsedTimer clock;
    QString stage = QStringLiteral("prepare");
    int nativeCode = 0;
    int inputTokens = -1, outputTokens = -1, reasoningTokens = -1, finalTextBytes = -1;

    explicit Impl(ConfiguredAiTextTransport *owner) : owner(owner) {
        timeout.setSingleShot(true);
        heartbeat.setInterval(1000);
        QObject::connect(&heartbeat, &QTimer::timeout, owner, [this] {
            if (active && !codex) emit this->owner->requestProgress(request.requestId, stage, clock.elapsed());
        });
        QObject::connect(&timeout, &QTimer::timeout, owner, [this] {
            if (active) fail(QStringLiteral("AI 请求超时，已完成的进度可手动继续。"), "timeout");
        });
#ifdef Q_OS_WIN
        QObject::connect(&native, &WinHttpModelTransport::networkDetails, owner, [this](const QString &phase, int code) {
            if (active && !codex) { stage=phase; nativeCode=code; }
        });
        QObject::connect(&native, &WinHttpModelTransport::replyReady, owner,
                         [this](int status, const QByteArray &contents, const QString &error) {
            if (active && !codex) parseReply(status, contents,
                    QString(error).replace(QStringLiteral("获取模型"), QStringLiteral("AI 请求"))
                                  .replace(QStringLiteral("模型列表响应"), QStringLiteral("AI 响应")));
        });
#endif
    }

    void stop() {
        ++generation;
        active = false;
        timeout.stop();
        heartbeat.stop();
#ifdef Q_OS_WIN
        native.cancel();
#endif
        if (reply) {
            QNetworkReply *previous = reply.data();
            reply.clear();
            previous->abort();
            previous->deleteLater();
        }
        if (codex) {
            CodexTextSession *previous = codex.data();
            codex.clear();
            previous->cancel();
        }
        response.clear();
    }

    void fail(const QString &message) {
        fail(message, "protocol");
    }
    void fail(const QString &message, const QString &category, int status = 0, bool retryable = true) {
        if (!active) return;
        const QString id = request.requestId;
        AiFailure error;
        error.requestId=id; error.jobId=request.jobId; error.stage=stage; error.category=category;
        error.message=message; error.httpStatus=status; error.nativeCode=nativeCode;
        error.elapsedMs=clock.isValid() ? clock.elapsed() : 0; error.retryable=retryable;
        error.maxOutputTokens=preferences.aiConnection=="api" && request.maxOutputTokens>0 ? request.maxOutputTokens : -1;
        error.inputTokens=inputTokens; error.outputTokens=outputTokens;
        error.reasoningTokens=reasoningTokens; error.finalTextBytes=finalTextBytes;
        logAiFailure(error);
        stop();
        emit owner->failureInfo(error);
        emit owner->failed(id, message);
    }

    void parseReply(int status, const QByteArray &contents, const QString &error) {
        if (status >= 300 && status < 400) {
            fail(QStringLiteral("API 地址发生重定向，请在设置中填写最终服务地址。"), "redirect", status);
            return;
        }
        if (status >= 400) { fail(httpError(status), aiHttpCategory(status), status); return; }
        if (!error.isEmpty()) { fail(error, error.contains(QStringLiteral("TLS")) ? "tls" : error.contains(QStringLiteral("超时")) ? "timeout" : "network"); return; }
        if (status < 200 || status >= 300 || contents.size() > maximumResponseSize) {
            fail(QStringLiteral("AI 服务响应无效或过大，未应用任何结果。"));
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
        const QJsonObject root = document.object();
        const QJsonArray choices = root.value("choices").toArray();
        if (parseError.error != QJsonParseError::NoError || !document.isObject() || choices.size() != 1) {
            fail(QStringLiteral("AI 服务未返回 OpenAI 兼容的完整文本响应。"));
            return;
        }
        const QJsonObject choice = choices.first().toObject();
        const QJsonObject message = choice.value("message").toObject();
        const auto usage = root.value("usage").toObject();
        inputTokens=tokenCount(usage.value("prompt_tokens"));
        outputTokens=tokenCount(usage.value("completion_tokens"));
        reasoningTokens=tokenCount(usage.value("completion_tokens_details").toObject().value("reasoning_tokens"));
        finalTextBytes=message.value("content").isString() ? message.value("content").toString().toUtf8().size() : -1;
        // Reject tool/refusal responses before deciding whether a length limit can be recovered.
        const bool unsafe=(!message.value("tool_calls").isNull() && !message.value("tool_calls").isUndefined()
                  && (!message.value("tool_calls").isArray() || !message.value("tool_calls").toArray().isEmpty()))
                || (!message.value("function_call").isNull() && !message.value("function_call").isUndefined())
                || !message.value("refusal").toString().isEmpty();
        if (!choice.value("message").isObject() || unsafe
            || (!message.value("content").isNull() && !message.value("content").isUndefined() && !message.value("content").isString())) {
            fail(QStringLiteral("AI 响应包含不支持的文本、拒绝或工具操作，未应用任何结果。")); return;
        }
        const QString reason=choice.value("finish_reason").toString();
        if (reason != "stop") {
            DiagnosticLog::instance().record("request.incomplete", {{"jobId",request.jobId},{"requestId",request.requestId},
                {"reason",reason=="length" || reason=="content_filter" ? reason : QString("unknown")},
                {"inputTokens",inputTokens}, {"outputTokens",outputTokens}, {"maxOutputTokens",request.maxOutputTokens},
                {"reasoningTokens",reasoningTokens}, {"finalTextBytes",finalTextBytes}});
            fail(reason=="length" ? QStringLiteral("模型输出达到长度上限，进度已保留。")
                                  : QStringLiteral("AI 响应未正常结束，未应用任何结果。"), reason=="length" ? "truncated" : "protocol", status);
            return;
        }
        if (!message.value("content").isString()
                || message.value("content").toString().trimmed().isEmpty()) {
            fail(QStringLiteral("AI 未返回可用的最终文本，或请求了工具操作，未应用任何结果。"));
            return;
        }
        AiTextResult result;
        result.requestId = request.requestId;
        result.text = message.value("content").toString();
        result.inputTokens=inputTokens; result.outputTokens=outputTokens;
        result.reasoningTokens=reasoningTokens; result.finalTextBytes=finalTextBytes;
        result.maxOutputTokens=request.maxOutputTokens>0 ? request.maxOutputTokens : -1;
        DiagnosticLog::instance().record("request.completed", {{"jobId", request.jobId}, {"requestId", request.requestId},
            {"elapsedMs", double(clock.elapsed())}, {"inputTokens", result.inputTokens}, {"outputTokens", result.outputTokens},
            {"maxOutputTokens", result.maxOutputTokens}, {"reasoningTokens", result.reasoningTokens}, {"finalTextBytes", result.finalTextBytes}});
        stop();
        emit owner->completed(result);
    }

    void complete(const AiTextRequest &incoming) {
        stop();
        request = incoming;
        request.timeoutMs = qBound(1, request.timeoutMs, 30 * 60000);
        request.maxOutputTokens = qBound(0, request.maxOutputTokens, 131072);
        const auto policy = policyFor(preferences);
        if (policy.maximumTokens > 0) request.maxOutputTokens = qMin(request.maxOutputTokens, policy.maximumTokens);
        active = true;
        clock.start(); stage="prepare"; nativeCode=0;
        inputTokens=outputTokens=reasoningTokens=finalTextBytes=-1;
        const QString validation = validatePreferences(preferences);
        if (!validation.isEmpty()) { fail(validation); return; }
        if (request.requestId.trimmed().isEmpty() || request.userPrompt.trimmed().isEmpty()
                || request.systemPrompt.toUtf8().size() + request.userPrompt.toUtf8().size()
                   + QJsonDocument(request.outputSchema).toJson(QJsonDocument::Compact).size() > maximumResponseSize) {
            fail(QStringLiteral("AI 请求为空或过大，未启动生成。"));
            return;
        }
        DiagnosticLog::instance().record("request.started", {{"jobId", request.jobId}, {"requestId", request.requestId},
            {"connection", preferences.aiConnection}, {"provider", preferences.providerId},
            {"model", preferences.aiConnection=="codex" ? preferences.codexModel : preferences.providers.value(preferences.providerId).model},
            {"proxyMode", preferences.networkProxy.mode}, {"timeoutMs", request.timeoutMs},
            {"maxOutputTokens", preferences.aiConnection=="api" ? request.maxOutputTokens : -1}});
        if (preferences.aiConnection=="api") DiagnosticLog::instance().record("request.endpoint", {{"jobId",request.jobId},
            {"requestId",request.requestId},{"host",QUrl(preferences.providers.value(preferences.providerId).baseUrl).host()}});
        const quint64 current = generation;
        if (preferences.aiConnection == "codex") {
            codex = new CodexTextSession(preferences, request,
                [this, current](const AiTextResult &result) {
                    if (!active || generation != current) return;
                    DiagnosticLog::instance().record("request.completed", {{"jobId", request.jobId},
                        {"requestId", request.requestId}, {"elapsedMs", double(clock.elapsed())},
                        {"maxOutputTokens", result.maxOutputTokens}, {"inputTokens", result.inputTokens},
                        {"outputTokens", result.outputTokens}, {"reasoningTokens", result.reasoningTokens}, {"finalTextBytes", result.finalTextBytes}});
                    codex.clear();
                    stop();
                    emit owner->completed(result);
                }, [this, current](const AiFailure &error) {
                    if (!active || generation != current) return;
                    codex.clear();
                    stop();
                    emit owner->failureInfo(error);
                    emit owner->failed(error.requestId, error.message);
                }, owner, [this, current](const QString &phase, qint64 elapsed) {
                    if (!active || generation != current) return;
                    stage=phase;
                    emit owner->requestProgress(request.requestId, phase, elapsed);
                });
            codex->start();
            return;
        }
        timeout.start(request.timeoutMs);
        heartbeat.start();
        stage="http.wait";
        const AiProviderConfig config = preferences.providers.value(preferences.providerId);
        QUrl url(config.baseUrl.trimmed(), QUrl::StrictMode);
        QString path = url.path();
        while (path.endsWith('/')) path.chop(1);
        if (!path.endsWith("/chat/completions")) path += "/chat/completions";
        url.setPath(path);
        QString system = request.systemPrompt;
        if (!request.outputSchema.isEmpty())
            system += QStringLiteral("\n最终文本必须是符合以下 JSON Schema 的 JSON，不要使用 Markdown 代码块：\n")
                    + QString::fromUtf8(QJsonDocument(request.outputSchema).toJson(QJsonDocument::Compact));
        QJsonObject parameters{{"model", config.model.trimmed()}, {"stream", false},
            {"messages", QJsonArray{QJsonObject{{"role", "system"}, {"content", system}},
                                      QJsonObject{{"role", "user"}, {"content", request.userPrompt}}}}};
        if (request.maxOutputTokens > 0)
            parameters.insert(preferences.providerId == "openai" ? "max_completion_tokens" : "max_tokens",
                              request.maxOutputTokens);
        if (policy.jsonOutput && !request.outputSchema.isEmpty()) parameters.insert("response_format", QJsonObject{{"type", "json_object"}});
        // Model-specific options are sent only for verified capability profiles.
        const QByteArray body = QJsonDocument(parameters).toJson(QJsonDocument::Compact);
#ifdef Q_OS_WIN
        native.post(url, config.apiKey.trimmed(), preferences.providerId, body,
                    qMax(1, request.timeoutMs-int(clock.elapsed())), preferences.networkProxy);
#else
        if (url.scheme() == "https" && !QSslSocket::supportsSsl()) {
            fail(QStringLiteral("当前程序缺少匹配的 HTTPS/TLS 运行库。"));
            return;
        }
        const bool loopback = url.host().compare("localhost", Qt::CaseInsensitive) == 0
                || QHostAddress(url.host()).isLoopback();
        if (loopback || preferences.networkProxy.mode=="direct") network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        else if (preferences.networkProxy.mode == "manual")
            network.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, preferences.networkProxy.host.trimmed(),
                                 static_cast<quint16>(preferences.networkProxy.port)));
        else {
            const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
            network.setProxy(proxies.isEmpty() ? QNetworkProxy(QNetworkProxy::NoProxy) : proxies.first());
        }
        QNetworkRequest http(url);
        http.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        http.setRawHeader("Accept", "application/json");
        http.setRawHeader("Content-Type", "application/json; charset=utf-8");
        http.setRawHeader("Authorization", "Bearer " + config.apiKey.trimmed().toUtf8());
        if (preferences.providerId == "mimo") http.setRawHeader("api-key", config.apiKey.trimmed().toUtf8());
        QNetworkReply *pending = network.post(http, body);
        reply = pending;
        pending->setReadBufferSize(maximumResponseSize + 1);
        QObject::connect(pending, &QNetworkReply::readyRead, owner, [this, pending, current] {
            if (!active || generation != current || reply != pending) return;
            response.append(pending->readAll());
            if (response.size() > maximumResponseSize)
                fail(QStringLiteral("AI 响应过大，已停止读取。"));
        });
        QObject::connect(pending, &QNetworkReply::finished, owner, [this, pending, current] {
            if (!active || generation != current || reply != pending) { pending->deleteLater(); return; }
            reply.clear();
            const int status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            response.append(pending->readAll());
            const auto error = pending->error();
            pending->deleteLater();
            const QByteArray contents = response;
            parseReply(status, contents, error == QNetworkReply::NoError ? QString()
                       : QStringLiteral("无法连接 AI 服务，请检查网络、代理和 TLS 配置。"));
        });
#endif
    }
};

ConfiguredAiTextTransport::ConfiguredAiTextTransport(QObject *parent)
    : AiTextTransport(parent), d(new Impl(this)) {
    qRegisterMetaType<AiTextRequest>();
    qRegisterMetaType<AiTextResult>();
    qRegisterMetaType<AiFailure>();
}

ConfiguredAiTextTransport::~ConfiguredAiTextTransport() { d->stop(); }
bool ConfiguredAiTextTransport::isAvailable() const { return validatePreferences(d->preferences).isEmpty(); }

void ConfiguredAiTextTransport::configure(const AppPreferences &preferences) {
    const bool before = isAvailable();
    const bool changed = !sameConnection(d->preferences, preferences);
    if (changed && d->active)
        d->fail(QStringLiteral("AI 设置已更改，进度已保留。"), "configuration");
    d->preferences = preferences;
    for (const auto &config : preferences.providers) DiagnosticLog::instance().addSecret(config.apiKey);
    if (changed) emit configurationChanged();
    if (before != isAvailable()) emit availabilityChanged();
}
int ConfiguredAiTextTransport::requestTimeoutMs() const { return qBound(1, d->preferences.requestTimeoutMinutes, 30)*60000; }
AiOutputPolicy ConfiguredAiTextTransport::outputPolicy() const { return policyFor(d->preferences); }
QString ConfiguredAiTextTransport::connectionIdentity() const {
    const auto &p=d->preferences;
    const auto config=p.providers.value(p.providerId);
    const QString identity=p.aiConnection=="codex" ? p.aiConnection+'\n'+p.codexExecutable+'\n'+p.codexModel
        : p.aiConnection+'\n'+p.providerId+'\n'+config.baseUrl.trimmed()+'\n'+config.model;
    return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void ConfiguredAiTextTransport::complete(const AiTextRequest &request) { d->complete(request); }
void ConfiguredAiTextTransport::cancel(const QString &requestId) {
    if (d->active && d->request.requestId == requestId) d->stop();
}
} // namespace lmsc
