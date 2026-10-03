#include "CodexTextSession.h"
#include "AppInfo.h"
#include "CodexAccountClient.h"

#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace lmsc {
namespace {
constexpr int maximumResponseSize = 4 * 1024 * 1024;
QString errorCategory(const QJsonObject &error) {
    const auto info=error.value("codexErrorInfo");
    const QString code=info.isString() ? info.toString() : info.toObject().keys().value(0);
    const QString evidence=code+' '+error.value("message").toString();
    if (evidence.contains("401") || evidence.contains("unauthor", Qt::CaseInsensitive) || evidence.contains("authentication", Qt::CaseInsensitive)) return "authentication";
    if (evidence.contains("403") || evidence.contains("permission", Qt::CaseInsensitive)) return "permission";
    if (evidence.contains("429") || evidence.contains("usageLimit", Qt::CaseInsensitive) || evidence.contains("rateLimit", Qt::CaseInsensitive)) return "quota";
    if (evidence.contains("proxy", Qt::CaseInsensitive)) return "proxy";
    if (evidence.contains("stream", Qt::CaseInsensitive) || evidence.contains("connect", Qt::CaseInsensitive)) return "network";
    if (evidence.contains("timeout", Qt::CaseInsensitive)) return "timeout";
    if (evidence.contains("contextWindow", Qt::CaseInsensitive) || evidence.contains("modelNotFound", Qt::CaseInsensitive)) return "parameters";
    if (evidence.contains("overload", Qt::CaseInsensitive) || evidence.contains("internalServer", Qt::CaseInsensitive)) return "server";
    return "protocol";
}
QString categoryLabel(const QString &category) {
    static const QHash<QString, QString> labels{{"authentication", QStringLiteral("账号认证")},
        {"permission", QStringLiteral("模型权限")}, {"quota", QStringLiteral("额度或频率限制")},
        {"proxy", QStringLiteral("代理连接")}, {"network", QStringLiteral("网络连接")},
        {"timeout", QStringLiteral("等待超时")}, {"parameters", QStringLiteral("模型或参数")},
        {"server", QStringLiteral("服务端异常")}, {"protocol", QStringLiteral("协议异常")}};
    return labels.value(category, QStringLiteral("未知异常"));
}

// These names are verified against the official 0.147.0 feature registry.
// Unknown CLI versions fail before inference instead of silently accepting a
// different tool contract. No user's config file is edited.
QStringList disabledFeatures() {
    return {"shell_tool", "unified_exec", "shell_snapshot", "hooks", "plugins",
            "remote_plugin", "apps", "enable_mcp_apps", "browser_use",
            "browser_use_external", "computer_use", "image_generation", "view_image",
            "multi_agent", "multi_agent_v2", "code_mode", "code_mode_only", "memories",
            "goals", "tool_suggest", "deferred_executor", "request_permissions_tool",
            "token_budget", "current_time_reminder", "skill_search",
            "skill_mcp_dependency_install", "tool_call_mcp_elicitation", "auth_elicitation",
            "workspace_dependencies", "guardian_approval", "guardianv2"};
}

QJsonObject securityConfiguration() {
    QJsonObject features;
    for (const QString &name : disabledFeatures()) features.insert(name, false);
    return {{"features", features}, {"approval_policy", "never"},
            {"approvals_reviewer", "user"}, {"sandbox_mode", "read-only"},
            {"model_provider", "openai"}, {"forced_login_method", "chatgpt"},
            {"web_search", "disabled"}, {"project_doc_max_bytes", 0},
            {"tools", QJsonObject{{"update_plan", QJsonObject{{"enabled", false}}},
                                  {"experimental_request_user_input", QJsonObject{{"enabled", false}}}}},
            {"skills", QJsonObject{{"include_instructions", false},
                                   {"bundled", QJsonObject{{"enabled", false}}}}}};
}

QStringList securityArguments() {
    QStringList result;
    for (const QString &name : disabledFeatures())
        result << "-c" << QStringLiteral("features.%1=false").arg(name);
    for (const QString &setting : {QStringLiteral("approval_policy=\"never\""),
          QStringLiteral("approvals_reviewer=\"user\""), QStringLiteral("sandbox_mode=\"read-only\""),
          QStringLiteral("model_provider=\"openai\""), QStringLiteral("forced_login_method=\"chatgpt\""),
          QStringLiteral("web_search=\"disabled\""), QStringLiteral("project_doc_max_bytes=0"),
          QStringLiteral("tools.update_plan.enabled=false"),
          QStringLiteral("tools.experimental_request_user_input.enabled=false"),
          QStringLiteral("skills.include_instructions=false"), QStringLiteral("skills.bundled.enabled=false")})
        result << "-c" << setting;
    result << "app-server" << "--stdio" << "--strict-config";
    return result;
}

bool disabled(const QJsonValue &value) {
    if (value.isBool()) return !value.toBool();
    return value.isObject() && value.toObject().value("enabled").isBool()
            && !value.toObject().value("enabled").toBool();
}

bool resolveExecutable(QString chosen, QString *program, QStringList *prefix) {
    if (chosen.trimmed().isEmpty()) chosen = CodexAccountClient::detectedExecutable();
    QFileInfo file(chosen.trimmed());
    if (!file.isFile()) return false;
    if (file.suffix().compare("cmd", Qt::CaseInsensitive) == 0
            || file.suffix().compare("bat", Qt::CaseInsensitive) == 0
            || file.suffix().compare("ps1", Qt::CaseInsensitive) == 0) {
        const QDir root(QDir(file.absolutePath()).filePath("node_modules/@openai"));
        QString native;
        const bool arm = qEnvironmentVariable("PROCESSOR_ARCHITECTURE").contains("ARM", Qt::CaseInsensitive)
                || qEnvironmentVariable("PROCESSOR_ARCHITEW6432").contains("ARM", Qt::CaseInsensitive);
        const QStringList targets = arm ? QStringList{"aarch64-pc-windows-msvc", "x86_64-pc-windows-msvc"}
                                         : QStringList{"x86_64-pc-windows-msvc", "aarch64-pc-windows-msvc"};
        for (const QString &target : targets) {
            const QString package = target.startsWith("aarch64") ? "codex-win32-arm64" : "codex-win32-x64";
            for (const QString &vendor : {QStringLiteral("codex/node_modules/@openai/") + package + "/vendor",
                                          package + "/vendor", QStringLiteral("codex/vendor")}) {
                for (const QString &folder : {QStringLiteral("bin"), QStringLiteral("codex")}) {
                    const QString candidate = root.filePath(vendor + '/' + target + '/' + folder + "/codex.exe");
                    if (QFileInfo(candidate).isFile()) { native = candidate; break; }
                }
                if (!native.isEmpty()) break;
            }
            if (!native.isEmpty()) break;
        }
        file.setFile(native.isEmpty() ? root.filePath("codex/bin/codex.js") : native);
        if (!file.isFile()) return false;
    }
    if (file.suffix().compare("js", Qt::CaseInsensitive) == 0) {
        *program = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (program->isEmpty()) return false;
        *prefix = QStringList{file.absoluteFilePath()};
    } else *program = file.absoluteFilePath();
    return true;
}

QProcessEnvironment childEnvironment(const NetworkProxyConfig &proxy) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (proxy.mode == "direct") {
        for (const QString &name : {QStringLiteral("HTTP_PROXY"), QStringLiteral("HTTPS_PROXY"),
             QStringLiteral("ALL_PROXY"), QStringLiteral("http_proxy"), QStringLiteral("https_proxy"), QStringLiteral("all_proxy")})
            environment.remove(name);
        environment.insert("NO_PROXY", "*"); environment.insert("no_proxy", "*");
        return environment;
    }
    if (proxy.mode != "manual") return environment;
    QUrl url;
    url.setScheme("http");
    url.setHost(proxy.host.trimmed());
    url.setPort(proxy.port);
    for (const QString &name : {QStringLiteral("HTTP_PROXY"), QStringLiteral("HTTPS_PROXY"),
          QStringLiteral("ALL_PROXY"), QStringLiteral("http_proxy"), QStringLiteral("https_proxy"),
          QStringLiteral("all_proxy")}) environment.insert(name, url.toString(QUrl::FullyEncoded));
    environment.insert("NO_PROXY", "localhost,127.0.0.1,::1");
    environment.insert("no_proxy", "localhost,127.0.0.1,::1");
    return environment;
}
} // namespace

struct CodexTextSession::Impl {
    CodexTextSession *owner;
    AppPreferences preferences;
    AiTextRequest request;
    Success success;
    Failure failure;
    Progress progress;
    QString phase = "version";
    qint64 lastReportedSecond = -1;
    QTemporaryDir directory;
    QProcess process;
    QTimer watchdog;
    QElapsedTimer clock;
    QString program;
    QStringList prefix;
    QByteArray buffer;
    QHash<int, QString> pending;
    QHash<QString, QString> finalMessages;
    QString threadId;
    QString turnId;
    int nextId = 1;
    int totalBytes = 0;
    qint64 phaseDeadline = 0;
    bool checkingVersion = true;
    bool active = true;
    bool turnRequested = false;
    bool cancelWaitingForTurn = false;

    Impl(CodexTextSession *owner, const AppPreferences &preferences, const AiTextRequest &request,
         Success success, Failure failure, Progress progress)
        : owner(owner), preferences(preferences), request(request), success(std::move(success)),
          failure(std::move(failure)), progress(std::move(progress)),
          directory(QDir(QDir::tempPath()).filePath(QStringLiteral("lmsc-codex-generation-XXXXXX"))) {
        directory.setAutoRemove(false);
#ifdef Q_OS_WIN
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
            arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            arguments->startupInfo->wShowWindow = SW_HIDE;
        });
#endif
        process.setProcessChannelMode(QProcess::SeparateChannels);
        QObject::connect(&process, &QProcess::started, owner, [this] {
            if (!active) { process.kill(); return; }
            if (checkingVersion) return;
            send("initialize", {{"clientInfo", QJsonObject{{"name", "lightsaber_score_generation"},
                  {"title", AppInfo::name()}, {"version", AppInfo::version()}}}});
        });
        QObject::connect(&process, &QProcess::readyReadStandardError, owner, [this] {
            // Logs may contain local configuration or authentication details.
            process.readAllStandardError();
        });
        QObject::connect(&process, &QProcess::readyReadStandardOutput, owner, [this] { read(); });
        QObject::connect(&process, &QProcess::errorOccurred, owner, [this](QProcess::ProcessError error) {
            if (active && error == QProcess::FailedToStart)
                fail(QStringLiteral("无法启动 Codex CLI，请检查设置中的可执行文件路径。"));
        });
        QObject::connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), owner,
                         [this](int code, QProcess::ExitStatus status) {
            if (!active) { this->owner->deleteLater(); return; }
            if (checkingVersion) {
                buffer.append(process.readAllStandardOutput());
                const QString version = QString::fromUtf8(buffer).trimmed();
                if (code != 0 || status != QProcess::NormalExit
                        || !QRegularExpression(QStringLiteral("^codex-cli\\s+0\\.147\\.0$"))
                                .match(version).hasMatch()) {
                    fail(QStringLiteral("当前 Codex CLI 版本未通过文本生成安全兼容检查；请使用 0.147.0。"));
                    return;
                }
                checkingVersion = false;
                DiagnosticLog::instance().record("codex.version", {{"jobId", this->request.jobId},
                    {"requestId", this->request.requestId}, {"cliVersion", "0.147.0"}});
                buffer.clear();
                totalBytes = 0;
                setPhase("startup", 60000);
                process.start(program, prefix + securityArguments(), QIODevice::ReadWrite);
                return;
            }
            const QString phase = pending.isEmpty() ? QStringLiteral("生成期间") : pending.constBegin().value();
            DiagnosticLog::instance().record("codex.exited", {{"jobId", this->request.jobId},
                {"requestId", this->request.requestId}, {"exitCode", code}, {"stage", this->phase}});
            fail(QStringLiteral("Codex 生成服务已退出（%1），可查看本次日志。").arg(phase), "process", 0, true);
        });
        watchdog.setInterval(100);
        QObject::connect(&watchdog, &QTimer::timeout, owner, [this] {
            if (!active) return;
            if (clock.elapsed() >= this->request.timeoutMs)
                fail(QStringLiteral("AI 请求超时（%1），已完成的进度可手动继续。").arg(phase), "timeout", 0, true);
            else if (phaseDeadline && clock.elapsed() >= phaseDeadline)
                fail(QStringLiteral("Codex 阶段超时（%1），可查看本次日志后继续。").arg(phase), "timeout", 0, true);
            else if (clock.elapsed()/1000 != lastReportedSecond) {
                lastReportedSecond=clock.elapsed()/1000;
                if (this->progress) this->progress(phase, clock.elapsed());
            }
        });
    }

    void start() {
        if (!directory.isValid() || !resolveExecutable(preferences.codexExecutable, &program, &prefix)) {
            fail(QStringLiteral("未找到可用的 Codex CLI，或无法创建临时生成目录。"));
            return;
        }
        process.setWorkingDirectory(directory.path());
        process.setProcessEnvironment(childEnvironment(preferences.networkProxy));
        clock.start();
        setPhase("version", 60000);
        watchdog.start();
        process.start(program, prefix + QStringList{"--version"}, QIODevice::ReadWrite);
    }

    void stop(bool interrupt, bool graceful = false) {
        if (!active) return;
        active = false;
        watchdog.stop();
        pending.clear();
        if (interrupt && !threadId.isEmpty() && turnRequested && process.state() == QProcess::Running) {
            if (!turnId.isEmpty()) sendInterrupt();
            else cancelWaitingForTurn = true;
            QTimer::singleShot(250, owner, [this] {
                if (process.state() != QProcess::NotRunning) process.kill();
                else owner->deleteLater();
            });
        } else if (graceful && process.state() == QProcess::Running) {
            // EOF lets app-server release its own local runtime before exit.
            // This never touches another Codex process or the user's config.
            process.closeWriteChannel();
            QTimer::singleShot(1000, owner, [this] {
                if (process.state() != QProcess::NotRunning) process.kill();
                else owner->deleteLater();
            });
        } else if (process.state() != QProcess::NotRunning) process.kill();
        else owner->deleteLater();
    }

    void sendInterrupt() {
        cancelWaitingForTurn = false;
        write({{"id", nextId++}, {"method", "turn/interrupt"},
               {"params", QJsonObject{{"threadId", threadId}, {"turnId", turnId}}}});
    }

    void setPhase(const QString &value, int budget = 60000) {
        phase=value;
        phaseDeadline=budget ? clock.elapsed()+budget : 0;
        DiagnosticLog::instance().record("codex.phase", {{"jobId", request.jobId}, {"requestId", request.requestId},
            {"stage", phase}, {"elapsedMs", double(clock.elapsed())}, {"timeoutMs", budget}});
        if (progress) progress(phase, clock.elapsed());
    }
    void fail(const QString &message, const QString &category = "protocol", int rpcCode = 0, bool retryable = false) {
        if (!active) return;
        AiFailure error;
        error.requestId=request.requestId; error.jobId=request.jobId; error.stage=phase;
        error.category=category; error.rpcCode=rpcCode; error.message=message;
        error.elapsedMs=clock.elapsed(); error.retryable=retryable;
        logAiFailure(error);
        stop(true);
        failure(error); // Only classified client-authored errors; never raw CLI output.
    }

    bool write(const QJsonObject &message) {
        const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        if (process.write(line) == line.size()) return true;
        if (active) fail(QStringLiteral("无法向 Codex 生成服务发送请求。"));
        return false;
    }

    void send(const QString &method, const QJsonObject &parameters) {
        const int id = nextId++;
        pending.insert(id, method);
        setPhase(method, method=="turn/start" ? 0 : 60000);
        write({{"id", id}, {"method", method}, {"params", parameters}});
    }

    void read() {
        const QByteArray bytes = process.readAllStandardOutput();
        if (!active && !cancelWaitingForTurn) return;
        totalBytes += bytes.size();
        if (totalBytes > (checkingVersion ? 1024 : maximumResponseSize)) {
            fail(QStringLiteral("Codex 返回的数据过大，已停止生成。"));
            return;
        }
        buffer.append(bytes);
        if (checkingVersion) return;
        int newline;
        while ((active || cancelWaitingForTurn) && (newline = buffer.indexOf('\n')) >= 0) {
            const QByteArray line = buffer.left(newline).trimmed();
            buffer.remove(0, newline + 1);
            if (line.isEmpty()) continue;
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                fail(QStringLiteral("Codex 返回了无效的协议数据，未应用任何结果。"));
                return;
            }
            handle(document.object());
        }
    }

    void handle(const QJsonObject &message) {
        const QString method = message.value("method").toString();
        // Cancellation can race the turn/start response. Briefly accept only
        // its identifier so the official interrupt reaches the server; no
        // result or callback from this cancelled generation is delivered.
        if (!active) {
            if (!cancelWaitingForTurn) return;
            if (!method.isEmpty() && message.contains("id")) {
                write({{"id", message.value("id")},
                       {"error", QJsonObject{{"code", -32601}, {"message", "Cancelled"}}}});
                cancelWaitingForTurn = false;
                process.kill();
                return;
            }
            QJsonObject turn;
            if (method.startsWith("turn/")
                    && message.value("params").toObject().value("threadId").toString() == threadId)
                turn = message.value("params").toObject().value("turn").toObject();
            else if (method.isEmpty()) turn = message.value("result").toObject().value("turn").toObject();
            const QString id = turn.value("id").toString();
            if (!id.isEmpty()) { turnId = id; sendInterrupt(); }
            return;
        }
        if (!method.isEmpty()) {
            if (message.contains("id")) {
                write({{"id", message.value("id")},
                       {"error", QJsonObject{{"code", -32601}, {"message", "Text generation rejects tool requests"}}}});
                fail(QStringLiteral("Codex 请求了文本生成之外的操作，已拒绝并停止生成。"));
                return;
            }
            notification(method, message.value("params").toObject());
            return;
        }
        if (!message.value("id").isDouble()) return;
        const int id = message.value("id").toInt();
        const QString requestMethod = pending.take(id);
        if (requestMethod.isEmpty()) return;
        if (message.contains("error") || !message.value("result").isObject()) {
            const QJsonObject error=message.value("error").toObject();
            const QString category=errorCategory(error);
            fail(QStringLiteral("Codex 请求失败（%1，%2），请查看本次日志。")
                 .arg(requestMethod, categoryLabel(category)), category, error.value("code").toInt(), true);
            return;
        }
        result(requestMethod, message.value("result").toObject());
    }

    void result(const QString &method, const QJsonObject &result) {
        if (method == "initialize") {
            write({{"method", "initialized"}, {"params", QJsonObject{}}});
            if (active) send("account/read", {{"refreshToken", false}});
        } else if (method == "account/read") {
            if (result.value("account").toObject().value("type").toString() != "chatgpt") {
                fail(QStringLiteral("请先在设置中完成 ChatGPT 账号授权。"), "authentication", 0, true);
                return;
            }
            send("config/read", {{"cwd", directory.path()}, {"includeLayers", false}});
        } else if (method == "config/read") {
            const QJsonObject config = result.value("config").toObject();
            const QJsonObject features = config.value("features").toObject();
            for (const QString &name : disabledFeatures()) {
                if (!disabled(features.value(name))) {
                    fail(QStringLiteral("无法确认 Codex 的工具已关闭，已停止生成。"));
                    return;
                }
            }
            if (config.value("sandbox_mode").toString() != "read-only"
                    || config.value("approval_policy").toString() != "never"
                    || config.value("web_search").toString() != "disabled") {
                fail(QStringLiteral("Codex 安全配置未生效，已停止生成。"));
                return;
            }
            // The public config/read ToolsV2 deliberately omits update_plan
            // and experimental_request_user_input. Their startup flags are
            // checked by --strict-config and the pinned core implementation;
            // treating an omitted public field as enabled rejects valid CLIs.
            if (config.contains("mcp_servers") && !config.value("mcp_servers").isObject()) {
                fail(QStringLiteral("无法确认 MCP 配置，已停止生成。"));
                return;
            }
            QJsonObject overrides = securityConfiguration();
            QJsonObject mcp;
            // Empty tables merge with existing MCP config instead of removing
            // it. Copy only server names, never headers, env or credentials.
            const QJsonObject configuredMcp = config.value("mcp_servers").toObject();
            for (auto it = configuredMcp.constBegin(); it != configuredMcp.constEnd(); ++it)
                mcp.insert(it.key(), QJsonObject{{"enabled", false}});
            overrides.insert("mcp_servers", mcp);
            send("thread/start", {{"model", preferences.codexModel.trimmed()}, {"modelProvider", "openai"},
                  {"cwd", directory.path()}, {"ephemeral", true}, {"sandbox", "read-only"},
                  {"approvalPolicy", "never"}, {"approvalsReviewer", "user"}, {"config", overrides},
                  {"baseInstructions", request.systemPrompt},
                  {"developerInstructions", "Return the requested JSON as the final text response. Tool use is disabled."}});
        } else if (method == "thread/start") {
            const QJsonObject sandbox = result.value("sandbox").toObject();
            threadId = result.value("thread").toObject().value("id").toString();
            if (threadId.isEmpty() || sandbox.value("type").toString() != "readOnly"
                    || (sandbox.contains("networkAccess")
                        && (!sandbox.value("networkAccess").isBool() || sandbox.value("networkAccess").toBool()))
                    || result.value("approvalPolicy").toString() != "never") {
                fail(QStringLiteral("Codex 会话没有采用只读限制，已停止生成。"));
                return;
            }
            turnRequested = true;
            QJsonObject parameters{{"threadId", threadId}, {"approvalPolicy", "never"},
                  {"sandboxPolicy", QJsonObject{{"type", "readOnly"}, {"networkAccess", false}}},
                  {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", request.userPrompt}}}}};
            if (!request.outputSchema.isEmpty()) parameters.insert("outputSchema", request.outputSchema);
            send("turn/start", parameters);
        } else if (method == "turn/start") {
            const QString id = result.value("turn").toObject().value("id").toString();
            if (id.isEmpty() || (!turnId.isEmpty() && turnId != id)) {
                fail(QStringLiteral("Codex 返回了不一致的生成会话，未应用任何结果。"));
                return;
            }
            turnId = id;
            phaseDeadline = 0;
        }
    }

    void notification(const QString &method, const QJsonObject &parameters) {
        if (method.startsWith("hook/") || method.contains("toolCall", Qt::CaseInsensitive)) {
            fail(QStringLiteral("Codex 尝试了文本生成之外的操作，已停止生成。"));
            return;
        }
        if (method != "item/started" && method != "item/completed"
                && method != "turn/started" && method != "turn/completed" && method != "error") return;
        if (parameters.value("threadId").toString() != threadId || !turnRequested) return;
        const QString notificationTurn = method.startsWith("turn/")
                ? parameters.value("turn").toObject().value("id").toString()
                : parameters.value("turnId").toString();
        if (notificationTurn.isEmpty()) {
            fail(QStringLiteral("Codex 生成通知缺少会话标识，未应用任何结果。"));
            return;
        }
        if (!turnId.isEmpty() && notificationTurn != turnId) return;
        if (turnId.isEmpty()) turnId = notificationTurn;
        if (method == "error") {
            const QString category=errorCategory(parameters.value("error").toObject());
            if (parameters.value("willRetry").isBool() && parameters.value("willRetry").toBool()) {
                setPhase("reconnecting", 0);
                DiagnosticLog::instance().record("codex.recovering", {{"jobId", request.jobId},
                    {"requestId", request.requestId}, {"category", category}, {"elapsedMs", double(clock.elapsed())}});
                return;
            }
            fail(QStringLiteral("Codex 模型生成失败（%1），可查看本次日志。").arg(categoryLabel(category)), category, 0, true);
            return;
        }
        if (method.startsWith("item/")) {
            const QJsonObject item = parameters.value("item").toObject();
            const QString type = item.value("type").toString();
            if (type != "agentMessage" && type != "userMessage" && type != "reasoning"
                    && type != "contextCompaction") {
                fail(QStringLiteral("Codex 尝试调用工具，已停止本次生成。"));
                return;
            }
            if (method != "item/completed" || type != "agentMessage") return;
            const QString phase = item.value("phase").toString();
            if (!phase.isEmpty() && phase != "final_answer") return;
            const QString id = item.value("id").toString();
            const QJsonValue text = item.value("text");
            if (id.isEmpty() || !text.isString() || text.toString().trimmed().isEmpty()) {
                fail(QStringLiteral("Codex 没有返回完整的最终文本。"));
                return;
            }
            if (finalMessages.contains(id) && finalMessages.value(id) != text.toString()) {
                fail(QStringLiteral("Codex 最终文本不一致，未应用任何结果。"));
                return;
            }
            finalMessages.insert(id, text.toString());
        } else if (method == "turn/completed") {
            const QJsonObject turn = parameters.value("turn").toObject();
            if (turn.value("status").toString()=="failed") {
                const QString category=errorCategory(turn.value("error").toObject());
                fail(QStringLiteral("Codex 生成失败（%1），可查看本次日志。").arg(categoryLabel(category)), category, 0, true);
                return;
            }
            if (turn.value("status").toString() != "completed" || finalMessages.size() != 1) {
                fail(QStringLiteral("Codex 生成未完整结束，未应用任何结果。"));
                return;
            }
            AiTextResult output;
            output.requestId = request.requestId;
            output.text = finalMessages.constBegin().value();
            output.finalTextBytes = output.text.toUtf8().size();
            stop(false, true);
            success(output);
        }
    }
};

CodexTextSession::CodexTextSession(const AppPreferences &preferences, const AiTextRequest &request,
                                 Success success, Failure failure, QObject *parent, Progress progress)
    : QObject(parent), d(new Impl(this, preferences, request, std::move(success), std::move(failure), std::move(progress))) {}

CodexTextSession::~CodexTextSession() {
    d->process.disconnect(this);
    if (d->process.state() != QProcess::NotRunning) {
        d->process.kill();
        d->process.waitForFinished(1000);
    }
    // Windows can release a child runtime's cwd handle just after app-server
    // exits. Remove only our empty QTemporaryDir, then retry once asynchronously
    // rather than recursively removing files or logging a misleading warning.
    const QString ownedPath = d->directory.path();
    if (!ownedPath.isEmpty() && !QDir().rmdir(ownedPath)
            && QCoreApplication::instance() && !QCoreApplication::closingDown())
        QTimer::singleShot(1500, QCoreApplication::instance(), [ownedPath] { QDir().rmdir(ownedPath); });
}

void CodexTextSession::start() { d->start(); }
void CodexTextSession::cancel() { d->stop(true); }

} // namespace lmsc
