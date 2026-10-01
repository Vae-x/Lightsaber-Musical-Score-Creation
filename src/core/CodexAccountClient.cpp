#include "CodexAccountClient.h"
#include "AppInfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace lmsc {
namespace {
QString nativeExecutableIn(const QString &openaiDirectory) {
#ifdef Q_OS_WIN
    const bool arm = qEnvironmentVariable("PROCESSOR_ARCHITECTURE").contains("ARM", Qt::CaseInsensitive)
        || qEnvironmentVariable("PROCESSOR_ARCHITEW6432").contains("ARM", Qt::CaseInsensitive);
    const QStringList targets = arm
        ? QStringList{QStringLiteral("aarch64-pc-windows-msvc"), QStringLiteral("x86_64-pc-windows-msvc")}
        : QStringList{QStringLiteral("x86_64-pc-windows-msvc"), QStringLiteral("aarch64-pc-windows-msvc")};
    for (const QString &target : targets) {
        const QString package = target.startsWith("aarch64") ? QStringLiteral("codex-win32-arm64")
                                                            : QStringLiteral("codex-win32-x64");
        const QStringList roots{QDir(openaiDirectory).filePath(package + "/vendor"),
                                QDir(openaiDirectory).filePath("codex/vendor")};
        for (const QString &root : roots) {
            const QString executable = QDir(root).filePath(target + "/codex/codex.exe");
            if (QFileInfo(executable).isFile()) return QFileInfo(executable).absoluteFilePath();
        }
    }
#else
    Q_UNUSED(openaiDirectory)
#endif
    return {};
}

QStringList npmRoots() {
    QStringList roots;
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) roots.append(QDir(appData).filePath("npm/node_modules/@openai"));
    const QStringList searchPaths = qEnvironmentVariable("PATH").split(QDir::listSeparator(), QString::SkipEmptyParts);
    for (const QString &path : searchPaths) {
        roots.append(QDir(path).filePath("node_modules/@openai"));
        roots.append(QDir(path).filePath("../lib/node_modules/@openai"));
    }
    roots.removeDuplicates();
    return roots;
}

QString safeError(QString message) {
    // 服务错误只显示短消息；不把凭据、登录 URL 或子进程输出写入日志。
    message.replace(QRegularExpression(QStringLiteral("Bearer\\s+[^\\s\"']+"),
                                      QRegularExpression::CaseInsensitiveOption), QStringLiteral("Bearer [已隐藏]"));
    message.replace(QRegularExpression(QStringLiteral("sk-[A-Za-z0-9_-]+")), QStringLiteral("[密钥已隐藏]"));
    return message.left(500);
}
} // namespace

CodexAccountClient::CodexAccountClient(QObject *parent) : QObject(parent) {
    m_clock.start();
#ifdef Q_OS_WIN
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
        arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        arguments->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_process, &QProcess::started, this, [this] {
        // 路径重配也可能发生在 Starting 阶段，旧进程不能再参与握手。
        if (m_stopping) {
            m_process.kill();
            return;
        }
        m_startDeadline = 0;
        const QJsonObject client{{"name", "lightsaber_score_settings"},
                                 {"title", AppInfo::name()}, {"version", AppInfo::version()}};
        sendRequest(QStringLiteral("initialize"), QJsonObject{{"clientInfo", client}});
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &CodexAccountClient::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        // stderr 可能含用户配置或认证信息，不保留、不显示。
        m_process.readAllStandardError();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_stopping && error == QProcess::FailedToStart)
            fail(QStringLiteral("无法启动 Codex CLI：%1").arg(safeError(m_process.errorString())));
    });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
        if (!m_stopping) {
            fail(QStringLiteral("Codex 账号服务已退出，请检查 CLI 路径后重试。"));
            return;
        }
        m_stopping = false;
        // 旧服务退出后再处理重配后的按钮操作，不在 UI 线程等待退出。
        // stop()/fail() 会清空队列，因此关闭和故障不会自行重启。
        if (!m_operations.isEmpty()) startServer();
    });
    m_watchdog.setInterval(500);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        const qint64 now = m_clock.elapsed();
        if (m_startDeadline && now > m_startDeadline) {
            fail(QStringLiteral("启动 Codex 账号服务超时，请检查 CLI 路径。"));
            return;
        }
        for (auto iterator = m_pending.constBegin(); iterator != m_pending.constEnd(); ++iterator) {
            if (now > iterator->deadline) {
                fail(QStringLiteral("Codex 账号服务请求超时（%1），请检查网络后重试。")
                     .arg(iterator->method));
                return;
            }
        }
    });
    m_loginTimeout.setSingleShot(true);
    m_loginTimeout.setInterval(5 * 60 * 1000);
    connect(&m_loginTimeout, &QTimer::timeout, this, [this] {
        cancelLogin();
        emit requestFailed(QStringLiteral("等待浏览器授权超时，请重新登录。"));
    });
}

CodexAccountClient::~CodexAccountClient() {
    stop();
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) m_process.waitForFinished(1000);
}

void CodexAccountClient::setExecutablePath(const QString &path) {
    const QString trimmed = path.trimmed();
    if (trimmed == m_executable) return;
    stop();
    m_executable = trimmed;
}

void CodexAccountClient::setProxyConfig(const NetworkProxyConfig &proxy) {
    if (m_proxy.mode == proxy.mode && m_proxy.host == proxy.host && m_proxy.port == proxy.port) return;
    stop();
    m_proxy = proxy;
}

QString CodexAccountClient::executablePath() const {
    return m_executable.isEmpty() ? detectedExecutable() : m_executable;
}

QString CodexAccountClient::detectedExecutable() {
#ifdef Q_OS_WIN
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("codex.exe"));
#else
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("codex"));
#endif
    if (!fromPath.isEmpty()) return fromPath;
    const QStringList roots = npmRoots();
    for (const QString &root : roots) {
        const QString native = nativeExecutableIn(root);
        if (!native.isEmpty()) return native;
    }
    for (const QString &root : roots) {
        const QString script = QDir(root).filePath(QStringLiteral("codex/bin/codex.js"));
        if (QFileInfo(script).isFile() && !QStandardPaths::findExecutable(QStringLiteral("node")).isEmpty())
            return QFileInfo(script).absoluteFilePath();
    }
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    const QStringList bundled{
        QDir(QCoreApplication::applicationDirPath()).filePath("tools/codex/codex.exe"),
        QDir(local).filePath("Programs/Codex/resources/codex.exe"),
        QDir(local).filePath("Codex/resources/codex.exe")};
    for (const QString &path : bundled) if (QFileInfo(path).isFile()) return path;
    return {};
}

bool CodexAccountClient::commandForPath(const QString &path, QString *program, QStringList *arguments) const {
    QFileInfo file(path);
    if (!file.isFile()) return false;
    QString chosen = file.absoluteFilePath();
    const QString extension = file.suffix().toLower();
    if (extension == "cmd" || extension == "ps1" || extension == "bat") {
        // npm 的入口通过原生程序或 Node 启动，不使用 cmd/PowerShell 拼接命令。
        const QString root = QDir(file.absolutePath()).filePath("node_modules/@openai");
        const QString native = nativeExecutableIn(root);
        chosen = native.isEmpty() ? QDir(root).filePath("codex/bin/codex.js") : native;
        if (!QFileInfo(chosen).isFile()) return false;
    }
    if (QFileInfo(chosen).suffix().compare("js", Qt::CaseInsensitive) == 0) {
        *program = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (program->isEmpty()) return false;
        *arguments = QStringList{chosen, QStringLiteral("app-server")};
    } else {
        *program = chosen;
        *arguments = QStringList{QStringLiteral("app-server")};
    }
    return true;
}

void CodexAccountClient::checkAccount() { enqueue(Operation::Account); }

void CodexAccountClient::beginLogin() {
    if (m_loginRequested) return;
    m_loginRequested = true;
    m_cancelLoginPending = false;
    enqueue(Operation::Login);
}

void CodexAccountClient::fetchModels() {
    if (m_fetchingModels) return;
    m_fetchingModels = true;
    m_models.clear();
    m_modelCursors.clear();
    enqueue(Operation::Models);
}

void CodexAccountClient::enqueue(Operation operation) {
    if (operation == Operation::Account && (m_operations.contains(operation)
        || hasRequest(QStringLiteral("account/read")))) return;
    m_operations.enqueue(operation);
    if (m_stopping && m_process.state() != QProcess::NotRunning) return;
    m_stopping = false;
    if (m_initialized) dispatchOperations();
    else if (m_process.state() == QProcess::NotRunning) startServer();
}

void CodexAccountClient::startServer() {
    const QString proxyError = AppSettings::validateProxy(m_proxy);
    if (!proxyError.isEmpty()) {
        fail(proxyError);
        return;
    }
    QString program;
    QStringList arguments;
    if (!commandForPath(executablePath(), &program, &arguments)) {
        fail(QStringLiteral("未找到可用的 Codex CLI。请安装官方 Codex CLI，或选择 codex.exe 路径。"));
        return;
    }
    m_output.clear();
    m_startDeadline = m_clock.elapsed() + 15000;
    m_watchdog.start();
    // Automatic mode preserves the official CLI's inherited proxy environment.
    // Manual mode overrides proxy variables only for this child process. Never
    // change qputenv(), the desktop Codex process, or Windows proxy preferences.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (m_proxy.mode == QStringLiteral("manual")) {
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(m_proxy.host.trimmed());
        url.setPort(m_proxy.port);
        const QString proxyUrl = url.toString(QUrl::FullyEncoded);
        for (const QString &name : {QStringLiteral("HTTP_PROXY"), QStringLiteral("HTTPS_PROXY"),
                                   QStringLiteral("ALL_PROXY"), QStringLiteral("http_proxy"),
                                   QStringLiteral("https_proxy"), QStringLiteral("all_proxy")})
            environment.insert(name, proxyUrl);
        // An inherited '*' bypass must not silently disable the selected proxy.
        // Keep only local destinations direct, including the browser callback.
        const QString bypass = QStringLiteral("localhost,127.0.0.1,::1");
        environment.insert(QStringLiteral("NO_PROXY"), bypass);
        environment.insert(QStringLiteral("no_proxy"), bypass);
    }
    m_process.setProcessEnvironment(environment);
    m_process.start(program, arguments, QIODevice::ReadWrite);
}

void CodexAccountClient::dispatchOperations() {
    while (m_initialized && !m_operations.isEmpty()) {
        switch (m_operations.dequeue()) {
        case Operation::Account:
            sendRequest(QStringLiteral("account/read"), QJsonObject{{"refreshToken", false}});
            break;
        case Operation::Login:
            if (m_loginRequested)
                sendRequest(QStringLiteral("account/login/start"), QJsonObject{{"type", "chatgpt"}});
            break;
        case Operation::Models:
            if (m_fetchingModels) requestModelsPage();
            break;
        }
    }
}

bool CodexAccountClient::hasRequest(const QString &method) const {
    for (const PendingRequest &request : m_pending) if (request.method == method) return true;
    return false;
}

int CodexAccountClient::sendRequest(const QString &method, const QJsonObject &parameters) {
    const int id = m_nextId++;
    m_pending.insert(id, PendingRequest{method, m_clock.elapsed() + 30000});
    sendMessage(QJsonObject{{"id", id}, {"method", method}, {"params", parameters}});
    return id;
}

void CodexAccountClient::sendMessage(const QJsonObject &message) {
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (m_process.write(line) != line.size())
        fail(QStringLiteral("无法向 Codex 账号服务发送请求。"));
}

void CodexAccountClient::readOutput() {
    const QByteArray bytes = m_process.readAllStandardOutput();
    if (m_stopping) return;
    m_output.append(bytes);
    // 非模型调用仅接收少量设置数据，避免异常子进程占用无限内存。
    if (m_output.size() > 2 * 1024 * 1024) {
        fail(QStringLiteral("Codex 账号服务返回的数据过大。"));
        return;
    }
    int newline = -1;
    while ((newline = m_output.indexOf('\n')) >= 0) {
        const QByteArray line = m_output.left(newline).trimmed();
        m_output.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("Codex 账号服务协议响应无效，请更新官方 CLI 后重试。"));
            return;
        }
        handleMessage(document.object());
        if (m_stopping) return;
    }
}

void CodexAccountClient::handleMessage(const QJsonObject &message) {
    const QString method = message.value("method").toString();
    if (!method.isEmpty()) {
        if (message.contains("id")) {
            // 此客户端不执行 turn，不接受服务端工具调用或令牌请求。
            sendMessage(QJsonObject{{"id", message.value("id")},
                                    {"error", QJsonObject{{"code", -32601}, {"message", "Unsupported request"}}}});
            return;
        }
        const QJsonObject parameters = message.value("params").toObject();
        if (method == "account/login/completed" && !m_loginId.isEmpty()
            && parameters.value("loginId").toString() == m_loginId) {
            const bool success = parameters.value("success").toBool();
            m_loginTimeout.stop();
            m_loginId.clear();
            m_loginRequested = false;
            m_cancelLoginPending = false;
            const QString error = safeError(parameters.value("error").toString());
            emit loginFinished(success, success ? QStringLiteral("ChatGPT 账号授权成功。")
                                                : QStringLiteral("账号授权未完成：%1").arg(error.isEmpty()
                                                        ? QStringLiteral("已取消或授权失败") : error));
            if (success) checkAccount();
        } else if (method == "account/updated") {
            checkAccount();
        }
        return;
    }
    if (!message.value("id").isDouble()) return;
    const int id = message.value("id").toInt();
    const auto iterator = m_pending.find(id);
    if (iterator == m_pending.end()) return;
    const QString request = iterator->method;
    m_pending.erase(iterator);
    if (message.contains("error")) {
        const QString detail = safeError(message.value("error").toObject().value("message").toString());
        const QString error = QStringLiteral("Codex 请求失败（%1）：%2").arg(request,
            detail.isEmpty() ? QStringLiteral("服务未提供错误详情") : detail);
        if (request == "initialize") { fail(error); return; }
        if (request.startsWith("account/login/")) {
            m_loginTimeout.stop();
            m_loginId.clear();
            m_loginRequested = false;
            m_cancelLoginPending = false;
            emit loginFinished(false, error);
        }
        if (request == "model/list") m_fetchingModels = false;
        emit requestFailed(error);
        return;
    }
    if (!message.value("result").isObject()) {
        fail(QStringLiteral("Codex 账号服务返回了无效的结果。"));
        return;
    }
    handleResult(request, message.value("result").toObject());
}

void CodexAccountClient::handleResult(const QString &method, const QJsonObject &result) {
    if (method == "initialize") {
        sendMessage(QJsonObject{{"method", "initialized"}, {"params", QJsonObject{}}});
        m_initialized = true;
        dispatchOperations();
    } else if (method == "account/read") {
        const QJsonValue accountValue = result.value("account");
        if (accountValue.isNull()) {
            emit accountStatus(QStringLiteral("尚未登录 ChatGPT 账号。"), false);
            return;
        }
        if (!accountValue.isObject() || accountValue.toObject().value("type").toString().isEmpty()) {
            fail(QStringLiteral("Codex 账号信息格式无效，请更新 CLI 后重试。"));
            return;
        }
        const QJsonObject account = accountValue.toObject();
        const QString type = account.value("type").toString();
        if (type != "chatgpt") {
            emit accountStatus(QStringLiteral("Codex 当前使用 %1，请登录 ChatGPT 账号以使用账号授权方式。")
                               .arg(type == "apiKey" ? QStringLiteral("API 密钥") : type), false);
            return;
        }
        QString description = QStringLiteral("ChatGPT 已登录");
        const QString email = account.value("email").toString();
        const QString plan = account.value("planType").toString();
        if (!email.isEmpty()) description += QStringLiteral(" · ") + email;
        if (!plan.isEmpty()) description += QStringLiteral("（%1）").arg(plan);
        emit accountStatus(description, true);
    } else if (method == "account/login/start") {
        const QUrl url(result.value("authUrl").toString(), QUrl::StrictMode);
        const QString loginId = result.value("loginId").toString();
        // 官方浏览器流程使用 HTTPS；不允许服务返回任意本地文件/命令。
        if (result.value("type").toString() != "chatgpt" || loginId.isEmpty()
            || !url.isValid() || url.scheme() != "https" || url.host().isEmpty()) {
            fail(QStringLiteral("Codex 返回的浏览器授权链接无效。"));
            return;
        }
        m_loginId = loginId;
        m_loginTimeout.start();
        if (m_cancelLoginPending) cancelLogin();
        else emit authorizationRequired(url);
    } else if (method == "account/login/cancel") {
        m_loginTimeout.stop();
        if (m_loginRequested) {
            m_loginId.clear();
            m_loginRequested = false;
            m_cancelLoginPending = false;
            emit loginFinished(false, QStringLiteral("已取消本次账号授权。"));
        }
    } else if (method == "model/list") {
        if (!result.value("data").isArray()) {
            fail(QStringLiteral("Codex 返回的模型列表格式无效。"));
            return;
        }
        for (const QJsonValue &value : result.value("data").toArray()) {
            const QJsonObject model = value.toObject();
            if (model.value("hidden").toBool()) continue;
            QString name = model.value("model").toString().trimmed();
            if (name.isEmpty()) name = model.value("id").toString().trimmed();
            if (!name.isEmpty() && !m_models.contains(name)) m_models.append(name);
        }
        const QString cursor = result.value("nextCursor").toString();
        if (!cursor.isEmpty()) {
            if (m_modelCursors.contains(cursor) || m_modelCursors.size() >= 100) {
                fail(QStringLiteral("Codex 模型分页游标异常。"));
                return;
            }
            m_modelCursors.insert(cursor);
            requestModelsPage(cursor);
        } else {
            m_fetchingModels = false;
            emit modelsReady(m_models);
        }
    }
}

void CodexAccountClient::requestModelsPage(const QString &cursor) {
    QJsonObject parameters{{"limit", 100}, {"includeHidden", false}};
    if (!cursor.isEmpty()) parameters.insert("cursor", cursor);
    sendRequest(QStringLiteral("model/list"), parameters);
}

void CodexAccountClient::cancelLogin() {
    if (!m_loginRequested) return;
    if (!m_loginId.isEmpty()) {
        if (!hasRequest(QStringLiteral("account/login/cancel")))
            sendRequest(QStringLiteral("account/login/cancel"), QJsonObject{{"loginId", m_loginId}});
        return;
    }
    if (hasRequest(QStringLiteral("account/login/start"))) {
        m_cancelLoginPending = true;
        return;
    }
    m_operations.removeAll(Operation::Login);
    m_loginRequested = false;
    m_cancelLoginPending = false;
    emit loginFinished(false, QStringLiteral("已取消本次账号授权。"));
}

void CodexAccountClient::resetState() {
    m_watchdog.stop();
    m_loginTimeout.stop();
    m_initialized = false;
    m_startDeadline = 0;
    m_pending.clear();
    m_operations.clear();
    m_output.clear();
    m_loginId.clear();
    m_loginRequested = false;
    m_cancelLoginPending = false;
    m_fetchingModels = false;
    m_models.clear();
    m_modelCursors.clear();
}

void CodexAccountClient::stop() {
    m_stopping = true;
    resetState();
    // 只关闭本对象启动的 app-server；不退出账号，不触碰桌面 Codex 的进程。
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
    else m_stopping = false;
}

void CodexAccountClient::fail(const QString &message) {
    const bool login = m_loginRequested;
    stop();
    if (login) emit loginFinished(false, message);
    emit requestFailed(message);
}

} // namespace lmsc
