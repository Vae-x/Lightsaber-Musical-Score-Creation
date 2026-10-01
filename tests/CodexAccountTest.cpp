#include "core/CodexAccountClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <functional>
#include <iostream>

namespace {
int failures = 0;

void check(bool condition, const QString &message) {
    if (condition) return;
    ++failures;
    QTextStream stream(stderr);
    stream.setCodec("UTF-8");
    stream << QStringLiteral("失败：") << message << '\n';
}

bool waitUntil(const std::function<bool()> &condition, int milliseconds = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }
    return condition();
}

void output(const QJsonObject &message) {
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    // 模拟 stdio 分片，客户端不能假定一次 readyRead 等于一条响应。
    const int midpoint = bytes.size() / 2;
    std::cout.write(bytes.constData(), midpoint);
    std::cout.flush();
    QThread::msleep(2);
    std::cout.write(bytes.constData() + midpoint, bytes.size() - midpoint);
    std::cout.flush();
}

int mockServer() {
    const QString mode = qEnvironmentVariable("LMSC_CODEX_TEST_MODE");
    QFile log(qEnvironmentVariable("LMSC_CODEX_TEST_LOG"));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return 3;
    const QString environmentLog = qEnvironmentVariable("LMSC_CODEX_TEST_ENV_LOG");
    if (!environmentLog.isEmpty()) {
        QFile file(environmentLog);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return 12;
        QJsonObject environment;
        for (const char *name : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy",
                                "https_proxy", "all_proxy", "NO_PROXY", "no_proxy"})
            environment.insert(QString::fromLatin1(name), qEnvironmentVariable(name));
        file.write(QJsonDocument(environment).toJson(QJsonDocument::Compact) + '\n');
    }
    bool initialized = false;
    bool handshake = false;
    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray bytes(line.data(), static_cast<int>(line.size()));
        log.write(bytes + '\n');
        log.flush();
        const QJsonObject request = QJsonDocument::fromJson(bytes).object();
        const QString method = request.value("method").toString();
        const QJsonValue id = request.value("id");
        const QJsonObject parameters = request.value("params").toObject();
        if (method == "initialize") {
            if (handshake || !parameters.value("clientInfo").isObject()) return 4;
            handshake = true;
            output(QJsonObject{{"id", id}, {"result", QJsonObject{{"userAgent", "mock"}}}});
        } else if (method == "initialized") {
            if (!handshake) return 5;
            initialized = true;
        } else if (!initialized) {
            return 6;
        } else if (method == "account/read") {
            if (!parameters.contains("refreshToken") || parameters.value("refreshToken").toBool()) return 7;
            QJsonValue account;
            if (mode == "loggedOut") account = QJsonValue(QJsonValue::Null);
            else if (mode == "apiKey") account = QJsonObject{{"type", "apiKey"}};
            else account = QJsonObject{{"type", "chatgpt"}, {"email", "test@example.invalid"}, {"planType", "plus"}};
            output(QJsonObject{{"id", id}, {"result", QJsonObject{{"account", account}, {"requiresOpenaiAuth", true}}}});
        } else if (method == "model/list") {
            if (mode == "rpcError") {
                output(QJsonObject{{"id", id}, {"error", QJsonObject{{"code", -1}, {"message", "Bad test sk-secret"}}}});
            } else if (mode == "malformed") {
                std::cout << "invalid JSON\n" << std::flush;
            } else if (parameters.value("cursor").toString().isEmpty()) {
                QJsonArray models{QJsonObject{{"id", "stable-id"}, {"model", "model-a"}},
                                  QJsonObject{{"id", "hidden-model"}, {"hidden", true}},
                                  QJsonObject{{"id", "stable-id"}, {"model", "model-a"}}};
                output(QJsonObject{{"id", id}, {"result", QJsonObject{{"data", models}, {"nextCursor", "page-2"}}}});
            } else if (parameters.value("cursor").toString() == "page-2") {
                QJsonArray models{QJsonObject{{"id", "model-b"}}, QJsonObject{{"model", "model-a"}}};
                output(QJsonObject{{"id", id}, {"result", QJsonObject{{"data", models},
                    {"nextCursor", mode == "cursorLoop" ? QJsonValue("page-2") : QJsonValue(QJsonValue::Null)}}}});
            } else return 8;
        } else if (method == "account/login/start") {
            if (parameters.value("type").toString() != "chatgpt") return 9;
            if (mode == "delayedCancel") QThread::msleep(250);
            output(QJsonObject{{"id", id}, {"result", QJsonObject{{"type", "chatgpt"}, {"loginId", "mock-login"},
                {"authUrl", mode == "invalidUrl" ? "file:///C:/test" : "https://auth.openai.com/mock"}}}});
            if (mode == "success") {
                output(QJsonObject{{"method", "account/login/completed"},
                    {"params", QJsonObject{{"loginId", "mock-login"}, {"success", true}, {"error", QJsonValue(QJsonValue::Null)}}}});
                output(QJsonObject{{"method", "account/updated"}, {"params", QJsonObject{{"authMode", "chatgpt"}}}});
            }
        } else if (method == "account/login/cancel") {
            if (parameters.value("loginId").toString() != "mock-login") return 10;
            output(QJsonObject{{"id", id}, {"result", QJsonObject{}}});
            output(QJsonObject{{"method", "account/login/completed"},
                {"params", QJsonObject{{"loginId", "mock-login"}, {"success", false}, {"error", "cancelled"}}}});
        } else {
            // 帐号设置不得偷偷创建会话、调用模型、退出既有账号或索取令牌。
            return 11;
        }
    }
    return 0;
}

void setup(lmsc::CodexAccountClient &client, const QString &mode, const QString &log) {
    qputenv("LMSC_CODEX_TEST_MODE", mode.toUtf8());
    qputenv("LMSC_CODEX_TEST_LOG", log.toUtf8());
    client.setExecutablePath(QCoreApplication::applicationFilePath());
}

QList<QJsonObject> requests(const QString &path) {
    QFile file(path);
    QList<QJsonObject> result;
    if (!file.open(QIODevice::ReadOnly)) return result;
    while (!file.atEnd()) result.append(QJsonDocument::fromJson(file.readLine()).object());
    return result;
}

void accountAndModelDiscovery(const QString &directory) {
    const QString log = QDir(directory).filePath("discovery.jsonl");
    lmsc::CodexAccountClient client;
    setup(client, "discovery", log);
    int accountCount = 0;
    bool loggedIn = false;
    QString status;
    QStringList models;
    int errors = 0;
    int authorizationCount = 0;
    QObject::connect(&client, &lmsc::CodexAccountClient::accountStatus, [&](const QString &message, bool current) {
        ++accountCount; loggedIn = current; status = message;
    });
    QObject::connect(&client, &lmsc::CodexAccountClient::modelsReady, [&](const QStringList &names) { models = names; });
    QObject::connect(&client, &lmsc::CodexAccountClient::requestFailed, [&](const QString &) { ++errors; });
    QObject::connect(&client, &lmsc::CodexAccountClient::authorizationRequired, [&](const QUrl &) { ++authorizationCount; });
    client.checkAccount();
    client.checkAccount();
    client.fetchModels();
    client.fetchModels();
    check(waitUntil([&] { return accountCount && !models.isEmpty(); }), QStringLiteral("异步帐号读取和模型分页完成"));
    check(loggedIn && status.contains("test@example.invalid") && status.contains("plus"), QStringLiteral("ChatGPT 帐号和套餐状态正确"));
    check(models == QStringList{"model-a", "model-b"}, QStringLiteral("使用 model 字段并兼容 id，去重并排除隐藏模型"));
    check(errors == 0 && authorizationCount == 0, QStringLiteral("读取设置不会自动发起登录"));
    int initializeCount = 0;
    int accountRequests = 0;
    int modelRequests = 0;
    for (const QJsonObject &request : requests(log)) {
        const QString method = request.value("method").toString();
        if (method == "initialize") ++initializeCount;
        else if (method == "account/read") ++accountRequests;
        else if (method == "model/list") ++modelRequests;
        check(method == "initialize" || method == "initialized" || method == "account/read" || method == "model/list",
              QStringLiteral("模型获取仅使用设置请求"));
    }
    check(initializeCount == 1 && accountRequests == 1 && modelRequests == 2,
          QStringLiteral("握手一次，合并重复点击，并发送下一页游标"));
}

void loginAndCancellation(const QString &directory, const QString &mode) {
    const QString log = QDir(directory).filePath(mode + ".jsonl");
    lmsc::CodexAccountClient client;
    setup(client, mode, log);
    int links = 0;
    int completions = 0;
    int accountCount = 0;
    bool successful = false;
    QUrl received;
    QObject::connect(&client, &lmsc::CodexAccountClient::authorizationRequired, [&](const QUrl &url) { ++links; received = url; });
    QObject::connect(&client, &lmsc::CodexAccountClient::loginFinished, [&](bool success, const QString &) {
        ++completions; successful = success;
    });
    QObject::connect(&client, &lmsc::CodexAccountClient::accountStatus, [&](const QString &, bool) { ++accountCount; });
    client.beginLogin();
    if (mode == "success") {
        check(waitUntil([&] { return completions && accountCount; }), QStringLiteral("成功通知后重新读取账号"));
        check(successful && links == 1 && received.scheme() == "https", QStringLiteral("浏览器授权和成功状态正确"));
    } else {
        check(waitUntil([&] { return links == 1; }), QStringLiteral("只有主动登录才返回浏览器链接"));
        client.cancelLogin();
        check(waitUntil([&] { return completions; }), QStringLiteral("取消授权完成"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        check(!successful && completions == 1, QStringLiteral("取消响应与完成通知不会重复报告"));
        bool cancelled = false;
        for (const QJsonObject &request : requests(log)) {
            const QString method = request.value("method").toString();
            cancelled |= method == "account/login/cancel";
            check(method != "account/logout", QStringLiteral("取消本次登录不会退出用户既有账号"));
        }
        check(cancelled, QStringLiteral("用此次 loginId 取消官方登录流程"));
    }
}

void accountModes(const QString &directory, const QString &mode) {
    lmsc::CodexAccountClient client;
    setup(client, mode, QDir(directory).filePath(mode + ".jsonl"));
    bool completed = false;
    bool loggedIn = true;
    QObject::connect(&client, &lmsc::CodexAccountClient::accountStatus, [&](const QString &, bool value) {
        completed = true; loggedIn = value;
    });
    client.checkAccount();
    check(waitUntil([&] { return completed; }) && !loggedIn,
          QStringLiteral("未登录和 API 密钥模式不会被误认为 ChatGPT 授权：") + mode);
}

void cancellationWhileStarting(const QString &directory) {
    const QString log = QDir(directory).filePath("delayed-cancel.jsonl");
    lmsc::CodexAccountClient client;
    setup(client, "delayedCancel", log);
    int links = 0;
    int completions = 0;
    QObject::connect(&client, &lmsc::CodexAccountClient::authorizationRequired, [&](const QUrl &) { ++links; });
    QObject::connect(&client, &lmsc::CodexAccountClient::loginFinished, [&](bool success, const QString &) {
        check(!success, QStringLiteral("启动中的授权取消不报告成功"));
        ++completions;
    });
    client.beginLogin();
    check(waitUntil([&] {
        for (const QJsonObject &request : requests(log))
            if (request.value("method").toString() == "account/login/start") return true;
        return false;
    }), QStringLiteral("mock 已收到登录请求但尚未返回链接"));
    client.cancelLogin();
    check(waitUntil([&] { return completions > 0; }), QStringLiteral("待返回 loginId 的授权也能取消"));
    check(completions == 1 && links == 0, QStringLiteral("取消后收到的旧链接不会弹出浏览器"));
}

void executableReconfiguration(const QString &directory) {
    const QString log = QDir(directory).filePath("reconfigured.jsonl");
#ifdef Q_OS_WIN
    const QString alternate = QDir(directory).filePath("alternate-codex.exe");
#else
    const QString alternate = QDir(directory).filePath("alternate-codex");
#endif
    check(QFile::copy(QCoreApplication::applicationFilePath(), alternate), QStringLiteral("复制第二个 mock 可执行路径"));
    lmsc::CodexAccountClient client;
    setup(client, "reconfiguration", log);
    int accounts = 0;
    int errors = 0;
    QObject::connect(&client, &lmsc::CodexAccountClient::accountStatus, [&](const QString &, bool) { ++accounts; });
    QObject::connect(&client, &lmsc::CodexAccountClient::requestFailed, [&](const QString &) { ++errors; });
    client.checkAccount();
    check(waitUntil([&] { return accounts == 1; }), QStringLiteral("重配前服务正在运行"));
    client.setExecutablePath(alternate);
    client.checkAccount(); // 同一个按钮回调中的操作，无等待、无需再点一次。
    check(waitUntil([&] { return accounts == 2 || errors; }), QStringLiteral("更改路径后首次账号检查自动排队完成"));
    check(accounts == 2 && errors == 0, QStringLiteral("异步关闭旧服务期间不会拒绝新操作"));
    int initializeCount = 0;
    int accountRequests = 0;
    for (const QJsonObject &request : requests(log)) {
        if (request.value("method").toString() == "initialize") ++initializeCount;
        if (request.value("method").toString() == "account/read") ++accountRequests;
    }
    check(initializeCount == 2 && accountRequests == 2, QStringLiteral("旧服务退出后新服务只启动一次并重新握手"));

    client.setExecutablePath(QCoreApplication::applicationFilePath());
    client.checkAccount();
    client.stop(); // 明确关闭须丢弃刚才排队的操作。
    waitUntil([] { return false; }, 300);
    check(accounts == 2 && errors == 0, QStringLiteral("明确停止会清空重配队列，不自动重启"));
    initializeCount = 0;
    for (const QJsonObject &request : requests(log))
        if (request.value("method").toString() == "initialize") ++initializeCount;
    check(initializeCount == 2, QStringLiteral("关闭后没有第三个服务进程"));
}

void failureModes(const QString &directory, const QString &mode) {
    lmsc::CodexAccountClient client;
    setup(client, mode, QDir(directory).filePath(mode + ".jsonl"));
    QString error;
    bool loginFinished = false;
    int links = 0;
    QObject::connect(&client, &lmsc::CodexAccountClient::requestFailed, [&](const QString &message) { error = message; });
    QObject::connect(&client, &lmsc::CodexAccountClient::authorizationRequired, [&](const QUrl &) { ++links; });
    QObject::connect(&client, &lmsc::CodexAccountClient::loginFinished, [&](bool, const QString &) { loginFinished = true; });
    if (mode == "invalidUrl") client.beginLogin();
    else client.fetchModels();
    check(waitUntil([&] { return !error.isEmpty(); }), QStringLiteral("异常协议返回可读错误：") + mode);
    check(!error.contains("sk-secret"), QStringLiteral("错误中的 API 密钥被隐藏"));
    if (mode == "invalidUrl") check(links == 0 && loginFinished, QStringLiteral("无效授权链接不进入浏览器"));
    if (mode == "rpcError") {
        bool account = false;
        QObject::connect(&client, &lmsc::CodexAccountClient::accountStatus, [&](const QString &, bool) { account = true; });
        client.checkAccount();
        check(waitUntil([&] { return account; }), QStringLiteral("单个请求错误后仍可继续读取账号"));
    }
}

void proxyEnvironmentAndReconfiguration(const QString &directory) {
    // Only synthetic proxy values enter the test log. Restore the caller's
    // original environment, and verify the client never edits it itself.
    const QStringList names{QStringLiteral("HTTP_PROXY"), QStringLiteral("HTTPS_PROXY"),
                            QStringLiteral("ALL_PROXY"), QStringLiteral("http_proxy"),
                            QStringLiteral("https_proxy"), QStringLiteral("all_proxy"),
                            QStringLiteral("NO_PROXY"), QStringLiteral("no_proxy"),
                            QStringLiteral("LMSC_CODEX_TEST_ENV_LOG")};
    const QProcessEnvironment original = QProcessEnvironment::systemEnvironment();
    const QString environmentLog = QDir(directory).filePath("proxy-environment.jsonl");
    for (const QString &name : names) {
        const QByteArray key = name.toLatin1();
        if (name == QStringLiteral("LMSC_CODEX_TEST_ENV_LOG")) qputenv(key.constData(), environmentLog.toUtf8());
        else qputenv(key.constData(), name.contains(QStringLiteral("NO_PROXY"), Qt::CaseInsensitive)
                     ? QByteArray("*") : QByteArray("http://parent-proxy.invalid:18080"));
    }
    {
        lmsc::CodexAccountClient client;
        setup(client, "proxyEnvironment", QDir(directory).filePath("proxy-reconfiguration.jsonl"));
        int accounts = 0;
        QString error;
        QObject::connect(&client, &lmsc::CodexAccountClient::accountStatus, [&](const QString &, bool) { ++accounts; });
        QObject::connect(&client, &lmsc::CodexAccountClient::requestFailed, [&](const QString &message) { error = message; });
        client.checkAccount();
        check(waitUntil([&] { return accounts == 1 || !error.isEmpty(); }) && accounts == 1,
              QStringLiteral("默认代理模式可启动继承环境的官方账号服务"));
        client.setProxyConfig({QStringLiteral("manual"), QStringLiteral("proxy.example.invalid"), 7890});
        client.checkAccount();
        check(waitUntil([&] { return accounts == 2 || !error.isEmpty(); }) && accounts == 2,
              QStringLiteral("修改代理时异步重启服务并完成首次操作"));
        client.setProxyConfig({});
        client.checkAccount();
        check(waitUntil([&] { return accounts == 3 || !error.isEmpty(); }) && accounts == 3,
              QStringLiteral("切回自动代理时移除本窗口的手动子进程环境"));
        const QList<QJsonObject> snapshots = requests(environmentLog);
        check(snapshots.size() == 3, QStringLiteral("代理变化只重新启动必要的三个 mock 服务"));
        if (snapshots.size() == 3) {
            for (const QString &name : names) {
                if (name == QStringLiteral("LMSC_CODEX_TEST_ENV_LOG")) continue;
                const bool bypass = name.contains(QStringLiteral("NO_PROXY"), Qt::CaseInsensitive);
                const QString inherited = bypass ? QStringLiteral("*") : QStringLiteral("http://parent-proxy.invalid:18080");
                check(snapshots.at(0).value(name).toString() == inherited
                      && snapshots.at(2).value(name).toString() == inherited,
                      QStringLiteral("自动模式保留 CLI 原环境：") + name);
                check(snapshots.at(1).value(name).toString() == (bypass
                      ? QStringLiteral("localhost,127.0.0.1,::1") : QStringLiteral("http://proxy.example.invalid:7890")),
                      QStringLiteral("手动代理覆盖子进程并保留本地登录回调直连：") + name);
                check(qEnvironmentVariable(name.toLatin1().constData()) == inherited,
                      QStringLiteral("手动代理没有污染主进程环境：") + name);
            }
        }
        client.setProxyConfig({QStringLiteral("manual"), {}, 0});
        client.checkAccount();
        check(waitUntil([&] { return !error.isEmpty(); }), QStringLiteral("无效手动代理不启动新的服务"));
        check(requests(environmentLog).size() == 3, QStringLiteral("无效代理没有启动第四个 mock 子进程"));
    }
    for (const QString &name : names) {
        const QByteArray key = name.toLatin1();
        if (original.contains(name)) qputenv(key.constData(), original.value(name).toUtf8());
        else qunsetenv(key.constData());
    }
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains("app-server")) return mockServer();
    QTemporaryDir temporary;
    check(temporary.isValid(), QStringLiteral("创建 mock 服务测试目录"));
    accountAndModelDiscovery(temporary.path());
    loginAndCancellation(temporary.path(), "success");
    loginAndCancellation(temporary.path(), "cancel");
    cancellationWhileStarting(temporary.path());
    executableReconfiguration(temporary.path());
    accountModes(temporary.path(), "loggedOut");
    accountModes(temporary.path(), "apiKey");
    failureModes(temporary.path(), "rpcError");
    failureModes(temporary.path(), "malformed");
    failureModes(temporary.path(), "cursorLoop");
    failureModes(temporary.path(), "invalidUrl");
    proxyEnvironmentAndReconfiguration(temporary.path());
    {
        lmsc::CodexAccountClient client;
        client.setExecutablePath(QDir(temporary.path()).filePath("missing-codex.exe"));
        bool failed = false;
        QObject::connect(&client, &lmsc::CodexAccountClient::requestFailed, [&](const QString &) { failed = true; });
        client.checkAccount();
        check(failed, QStringLiteral("不存在的 CLI 路径立即给出可读错误"));
    }
    qunsetenv("LMSC_CODEX_TEST_MODE");
    qunsetenv("LMSC_CODEX_TEST_LOG");
    QTextStream stream(stdout);
    stream.setCodec("UTF-8");
    stream << (failures ? QStringLiteral("Codex 账号测试失败：%1").arg(failures)
                        : QStringLiteral("Codex 账号测试全部通过（仅 mock，不调用模型和真实登录）。")) << '\n';
    return failures ? 1 : 0;
}
