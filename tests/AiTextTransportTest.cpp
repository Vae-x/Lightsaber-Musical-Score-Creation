#include "core/AiTextTransport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <cstdio>

namespace {
struct Response {
    int status = 200;
    QByteArray body = R"({"choices":[{"finish_reason":"stop","message":{"content":"{\"notes\":[]}"}}],"usage":{"prompt_tokens":42,"completion_tokens":8}})";
    QByteArray headers;
    int delayMs = 0;
};

class HttpFixture : public QObject {
public:
    QTcpServer server;
    QList<QByteArray> requests;
    QList<Response> responses;
    HttpFixture() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                socket->setParent(this);
                const auto bytes = QSharedPointer<QByteArray>::create();
                const auto recorded = QSharedPointer<bool>::create(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes, recorded] {
                    bytes->append(socket->readAll());
                    const int end = bytes->indexOf("\r\n\r\n");
                    if (*recorded || end < 0) return;
                    int length = 0;
                    for (const QByteArray &header : bytes->left(end).split('\n'))
                        if (header.toLower().startsWith("content-length:")) length = header.mid(15).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    *recorded = true;
                    requests.append(*bytes);
                    const Response response = responses.isEmpty() ? Response{} : responses.takeFirst();
                    if (response.delayMs < 0) return;
                    QTimer::singleShot(response.delayMs, socket, [socket, response] {
                        if (socket->state() == QAbstractSocket::UnconnectedState) return;
                        socket->write("HTTP/1.1 " + QByteArray::number(response.status)
                              + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                              + QByteArray::number(response.body.size()) + "\r\n" + response.headers + "\r\n" + response.body);
                        socket->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }
    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    lmsc::AppPreferences preferences(const QString &provider = "deepseek") const {
        lmsc::AppPreferences result;
        result.providerId = provider;
        result.providers[provider] = {QStringLiteral("http://127.0.0.1:%1/v1/").arg(server.serverPort()),
                                    "test-only-api-key", "test-model", {}};
        return result;
    }
};

lmsc::AiTextRequest request(const QString &id = "request-1") {
    lmsc::AiTextRequest result;
    result.requestId = id;
    result.systemPrompt = QStringLiteral("根据音乐特征生成曲谱 JSON。");
    result.userPrompt = QStringLiteral("八小节，流畅的左右手交替。");
    result.outputSchema = {{"type", "object"}, {"properties", QJsonObject{{"notes", QJsonObject{{"type", "array"}}}}},
                           {"required", QJsonArray{"notes"}}, {"additionalProperties", false}};
    return result;
}

QJsonObject httpBody(const QByteArray &bytes) {
    return QJsonDocument::fromJson(bytes.mid(bytes.indexOf("\r\n\r\n") + 4)).object();
}

QList<QJsonObject> readLog(const QString &path) {
    QList<QJsonObject> result;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly))
        while (!file.atEnd()) result.append(QJsonDocument::fromJson(file.readLine()).object());
    return result;
}

bool loggedMethod(const QString &path, const QString &method) {
    for (const auto &entry : readLog(path)) if (entry.value("method").toString() == method) return true;
    return false;
}

void output(const QJsonObject &message, bool split = false) {
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (split) {
        fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size() / 2), stdout);
        fflush(stdout);
        QThread::msleep(10);
        fwrite(bytes.constData() + bytes.size() / 2, 1, static_cast<size_t>(bytes.size() - bytes.size() / 2), stdout);
    } else fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    fflush(stdout);
}

int mockServer() {
    const QString mode = qEnvironmentVariable("LMSC_TEXT_TEST_MODE");
    QFile log(qEnvironmentVariable("LMSC_TEXT_TEST_LOG"));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return 5;
    auto record = [&](const QJsonObject &message) {
        log.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n'); log.flush();
    };
    record({{"method", "mock/startup"}, {"arguments", QJsonArray::fromStringList(QCoreApplication::arguments())},
            {"cwd", QDir::currentPath()}, {"directoryEmpty", QDir::current().entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()}});
    QByteArray line;
    bool initialized = false;
    while (true) {
        line.clear();
        int character;
        while ((character = std::getchar()) != EOF && character != '\n') line.append(static_cast<char>(character));
        if (character == EOF && line.isEmpty()) break;
        const QJsonObject message = QJsonDocument::fromJson(line).object();
        record(message);
        const QString method = message.value("method").toString();
        const QJsonValue id = message.value("id");
        const QJsonObject params = message.value("params").toObject();
        auto reply = [&](const QJsonObject &result) { output({{"id", id}, {"result", result}}, mode == "split"); };
        auto event = [&](const QString &name, const QJsonObject &extra) {
            QJsonObject payload = extra;
            payload.insert("threadId", "mock-thread");
            if (name.startsWith("item/") || name=="error") payload.insert("turnId", "mock-turn");
            output({{"method", name}, {"params", payload}}, mode == "split");
        };
        if (method == "initialize") reply({{"userAgent", "mock"}});
        else if (method == "initialized") initialized = true;
        else if (!initialized) return 6;
        else if (method == "account/read")
            reply({{"account", mode == "loggedOut" ? QJsonValue(QJsonValue::Null)
                               : QJsonValue(QJsonObject{{"type", "chatgpt"}})}});
        else if (method == "config/read") {
            QJsonObject features;
            const QStringList arguments = QCoreApplication::arguments();
            for (int i = 0; i + 1 < arguments.size(); ++i) {
                const QString value = arguments.at(i + 1);
                if (arguments.at(i) == "-c" && value.startsWith("features.") && value.endsWith("=false"))
                    features.insert(value.mid(9, value.size() - 15), false);
            }
            if (mode == "unsafeConfig") features.insert("shell_tool", true);
            reply({{"config", QJsonObject{{"features", features}, {"sandbox_mode", "read-only"},
                   {"approval_policy", "never"}, {"web_search", "disabled"},
                   {"tools", QJsonObject{{"update_plan", QJsonObject{{"enabled", false}}},
                                         {"experimental_request_user_input", QJsonObject{{"enabled", false}}}}},
                   {"mcp_servers", QJsonObject{{"test.server", QJsonObject{{"command", "never-launch-this"},
                                                                       {"http_headers", QJsonObject{{"secret", "test-only-secret"}}}}}}}}}});
        } else if (method == "thread/start") {
            QJsonObject sandbox{{"type", mode == "unsafeThread" ? "dangerFullAccess" : "readOnly"}};
            if (mode != "noNetworkField") sandbox.insert("networkAccess", mode == "unsafeNetwork");
            reply({{"thread", QJsonObject{{"id", "mock-thread"}}}, {"approvalPolicy", "never"},
                   {"sandbox", sandbox}});
        } else if (method == "turn/start") {
            if (!params.value("outputSchema").isObject()) return 7;
            if (mode=="delayedTurnStart") QThread::msleep(200);
            reply({{"turn", QJsonObject{{"id", "mock-turn"}, {"status", "inProgress"}}}});
            event("turn/started", {{"turn", QJsonObject{{"id", "mock-turn"}, {"status", "inProgress"}}}});
            if (mode == "pause") continue;
            if (mode=="recovering" || mode=="permanentError") {
                event("error",{{"willRetry",mode=="recovering"},{"error",QJsonObject{{"codexErrorInfo","responseStreamDisconnected"},
                    {"message","test-only-secret connection lost"}}}});
                if (mode=="permanentError") continue;
            }
            if (mode=="staleError") output({{"method","error"},{"params",QJsonObject{{"threadId","mock-thread"},
                {"turnId","old-turn"},{"willRetry",false},{"error",QJsonObject{{"message","old failure"}}}}}});
            if (mode == "toolRequest") {
                output({{"id", "server-tool-request"}, {"method", "item/commandExecution/requestApproval"},
                        {"params", QJsonObject{{"threadId", "mock-thread"}, {"turnId", "mock-turn"}}}});
                continue;
            }
            if (mode == "toolItem") {
                event("item/started", {{"item", QJsonObject{{"type", "commandExecution"}, {"id", "command"}}}});
                continue;
            }
            event("item/completed", {{"item", QJsonObject{{"type", "agentMessage"}, {"id", "commentary"},
                   {"phase", "commentary"}, {"text", "Ignored commentary"}}}});
            event("item/completed", {{"item", QJsonObject{{"type", "agentMessage"}, {"id", "answer"},
                   {"phase", "final_answer"}, {"text", "{\"notes\":[]}"}}}});
            if (mode == "multipleFinal")
                event("item/completed", {{"item", QJsonObject{{"type", "agentMessage"}, {"id", "answer-2"},
                       {"phase", "final_answer"}, {"text", "{\"notes\":[1]}"}}}});
            event("turn/completed", {{"turn", QJsonObject{{"id", "mock-turn"},
                     {"status", mode == "failedTurn" ? "failed" : "completed"}}}});
        } else if (method == "turn/interrupt") {
            reply({});
            event("turn/completed", {{"turn", QJsonObject{{"id", "mock-turn"}, {"status", "interrupted"}}}});
        } else if (message.contains("error")) continue;
        else return 8; // No login/logout/MCP/tool/thread-resume methods allowed.
    }
    return 0;
}

lmsc::AppPreferences codexPreferences(const QString &mode, const QString &log) {
    qputenv("LMSC_TEXT_TEST_MODE", mode.toUtf8());
    qputenv("LMSC_TEXT_TEST_LOG", log.toUtf8());
    lmsc::AppPreferences preferences;
    preferences.aiConnection = "codex";
    preferences.codexExecutable = QCoreApplication::applicationFilePath();
    preferences.codexModel = "test-codex-model";
    return preferences;
}
} // namespace

class AiTextTransportTest : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { qunsetenv("LMSC_TEXT_TEST_MODE"); qunsetenv("LMSC_TEXT_TEST_LOG"); }
    void apiBodyAndCompletedText();
    void providerParameters_data();
    void providerParameters();
    void invalidResponse_data();
    void invalidResponse();
    void errorsNeverRetry_data();
    void errorsNeverRetry();
    void cancelledAndReconfiguredRepliesAreDiscarded();
    void responseLimitAndTimeout();
    void validationPreventsNetwork();
    void codexCompletedTextAndSecurity();
    void codexCompletedTextAndSecurity_data();
    void codexFailures_data();
    void codexFailures();
    void codexCancelAndTimeout();
    void codexNpmWrapperUsesNestedBinary();
};

void AiTextTransportTest::apiBodyAndCompletedText() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(fixture.preferences());
    QVERIFY(transport.isAvailable());
    QSignalSpy completed(&transport, &lmsc::AiTextTransport::completed);
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    auto input = request();
    input.userPrompt = QString(65536, QChar(0x66f2)) + QStringLiteral("谱尾部不可丢失");
    transport.complete(input);
    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(fixture.requests.size(), 1);
    const QByteArray wire = fixture.requests.first();
    QVERIFY(wire.startsWith("POST /v1/chat/completions HTTP/1.1"));
    QVERIFY(wire.toLower().contains("content-type: application/json"));
    QVERIFY(wire.contains("Authorization: Bearer test-only-api-key"));
    const QJsonObject body = httpBody(wire);
    QCOMPARE(body.value("messages").toArray().at(1).toObject().value("content").toString(), input.userPrompt);
    QVERIFY(body.value("messages").toArray().first().toObject().value("content").toString().contains("JSON Schema"));
    QCOMPARE(body.value("max_tokens").toInt(), 8192);
    QVERIFY(!body.contains("tools") && !body.contains("temperature") && !body.contains("response_format"));
    const auto result = qvariant_cast<lmsc::AiTextResult>(completed.first().first());
    QCOMPARE(result.requestId, input.requestId);
    QCOMPARE(result.text, QStringLiteral("{\"notes\":[]}"));
    QCOMPARE(result.inputTokens, 42);
    QCOMPARE(result.outputTokens, 8);
}

void AiTextTransportTest::providerParameters_data() {
    QTest::addColumn<QString>("provider");
    QTest::newRow("openai") << QStringLiteral("openai");
    QTest::newRow("kimi") << QStringLiteral("kimi");
    QTest::newRow("mimo") << QStringLiteral("mimo");
}
void AiTextTransportTest::providerParameters() {
    QFETCH(QString, provider);
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(fixture.preferences(provider));
    QSignalSpy completed(&transport, &lmsc::AiTextTransport::completed);
    transport.complete(request());
    QTRY_COMPARE(completed.count(), 1);
    const QJsonObject body = httpBody(fixture.requests.first());
    QVERIFY(body.contains(provider == "openai" ? "max_completion_tokens" : "max_tokens"));
    QVERIFY(!body.contains(provider == "openai" ? "max_tokens" : "max_completion_tokens"));
    QCOMPARE(fixture.requests.first().contains("api-key: test-only-api-key"), provider == "mimo");
}

void AiTextTransportTest::invalidResponse_data() {
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("truncated") << QByteArray(R"({"choices":[{"finish_reason":"length","message":{"content":"partial"}}]})");
    QTest::newRow("tool") << QByteArray(R"({"choices":[{"finish_reason":"stop","message":{"content":"{}","tool_calls":[{}]}}]})");
    QTest::newRow("function") << QByteArray(R"({"choices":[{"finish_reason":"stop","message":{"content":"{}","function_call":{}}}]})");
    QTest::newRow("refusal") << QByteArray(R"({"choices":[{"finish_reason":"stop","message":{"content":"{}","refusal":"refused"}}]})");
    QTest::newRow("content-parts") << QByteArray(R"({"choices":[{"finish_reason":"stop","message":{"content":[{"text":"{}"}]}}]})");
    QTest::newRow("malformed") << QByteArray("not json test-only-api-key");
    QTest::newRow("no-final-reason") << QByteArray(R"({"choices":[{"message":{"content":"{}"}}]})");
}
void AiTextTransportTest::invalidResponse() {
    QFETCH(QByteArray, body);
    HttpFixture fixture;
    fixture.responses.append({200, body, {}, 0});
    QVERIFY(fixture.listen());
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(fixture.preferences());
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    QSignalSpy completed(&transport, &lmsc::AiTextTransport::completed);
    QSignalSpy details(&transport, &lmsc::AiTextTransport::failureInfo);
    transport.complete(request());
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(completed.count(), 0);
    QVERIFY(!failed.first().at(1).toString().contains("test-only-api-key"));
    QCOMPARE(details.count(),1);
    QCOMPARE(qvariant_cast<lmsc::AiFailure>(details.first().first()).category,
             body.contains("\"length\"") ? QString("truncated") : QString("protocol"));
}

void AiTextTransportTest::errorsNeverRetry_data() {
    QTest::addColumn<int>("status");
    QTest::newRow("bad-optional-parameter") << 400;
    QTest::newRow("rate-limit") << 429;
    QTest::newRow("redirect") << 307;
    QTest::newRow("authentication") << 401;
    QTest::newRow("permission") << 403;
    QTest::newRow("proxy-authentication") << 407;
    QTest::newRow("server-error") << 503;
}
void AiTextTransportTest::errorsNeverRetry() {
    QFETCH(int, status);
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    fixture.responses.append({status, "test-only-api-key echoed by server",
                              "Location: " + fixture.preferences().providers["deepseek"].baseUrl.toUtf8() + "\r\n", 0});
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(fixture.preferences());
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    QSignalSpy details(&transport, &lmsc::AiTextTransport::failureInfo);
    transport.complete(request());
    QTRY_COMPARE(failed.count(), 1);
    QTest::qWait(100);
    QCOMPARE(fixture.requests.size(), 1);
    QVERIFY(!failed.first().at(1).toString().contains("test-only-api-key"));
    QCOMPARE(details.count(),1);
    const auto error=qvariant_cast<lmsc::AiFailure>(details.first().first());
    QCOMPARE(error.httpStatus,status);
    QCOMPARE(error.category,status==307 ? QString("redirect") : lmsc::aiHttpCategory(status));
    QVERIFY(!error.stage.isEmpty());
}

void AiTextTransportTest::cancelledAndReconfiguredRepliesAreDiscarded() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    fixture.responses = {{200, Response{}.body, {}, 350}, {200, Response{}.body, {}, 350}, {}};
    lmsc::ConfiguredAiTextTransport transport;
    auto preferences = fixture.preferences();
    transport.configure(preferences);
    QSignalSpy completed(&transport, &lmsc::AiTextTransport::completed);
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    transport.complete(request("cancelled"));
    QTRY_COMPARE(fixture.requests.size(), 1);
    transport.cancel("unrelated");
    transport.cancel("cancelled");
    transport.complete(request("changed"));
    QTRY_COMPARE(fixture.requests.size(), 2);
    preferences.providers["deepseek"].model = "new-model";
    transport.configure(preferences);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("changed"));
    transport.complete(request("current"));
    QTRY_COMPARE(completed.count(), 1);
    QTest::qWait(400);
    QCOMPARE(completed.count(), 1);
    QCOMPARE(qvariant_cast<lmsc::AiTextResult>(completed.first().first()).requestId, QStringLiteral("current"));
    QCOMPARE(failed.count(), 1);
}

void AiTextTransportTest::responseLimitAndTimeout() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    fixture.responses = {{200, QByteArray(4 * 1024 * 1024 + 1, 'x'), {}, 0}, {200, {}, {}, -1}};
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(fixture.preferences());
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    transport.complete(request());
    QTRY_COMPARE(failed.count(), 1);
    auto input = request("timeout");
    input.timeoutMs = 80;
    transport.complete(input);
    QTRY_COMPARE(failed.count(), 2);
    QCOMPARE(failed.at(1).first().toString(), QStringLiteral("timeout"));
    QVERIFY(failed.at(1).at(1).toString().contains(QStringLiteral("超时")));
}

void AiTextTransportTest::validationPreventsNetwork() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    lmsc::ConfiguredAiTextTransport transport;
    auto preferences = fixture.preferences();
    preferences.providers["deepseek"].apiKey = "bad\r\nheader";
    transport.configure(preferences);
    QVERIFY(!transport.isAvailable());
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    transport.complete(request());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(fixture.requests.size(), 0);
}

void AiTextTransportTest::codexCompletedTextAndSecurity_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("split-lines") << QStringLiteral("split");
    QTest::newRow("read-only-omits-network-default") << QStringLiteral("noNetworkField");
    QTest::newRow("recoverable-error-completes") << QStringLiteral("recovering");
    QTest::newRow("stale-turn-error-ignored") << QStringLiteral("staleError");
    QTest::newRow("delayed-turn-start") << QStringLiteral("delayedTurnStart");
}
void AiTextTransportTest::codexCompletedTextAndSecurity() {
    QFETCH(QString, mode);
    QTemporaryDir directory;
    const QString log = directory.filePath("codex.jsonl");
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(codexPreferences(mode, log));
    QSignalSpy completed(&transport, &lmsc::AiTextTransport::completed);
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    transport.complete(request());
    QTRY_VERIFY(completed.count() || failed.count());
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QCOMPARE(completed.count(), 1);
    QCOMPARE(qvariant_cast<lmsc::AiTextResult>(completed.first().first()).text, QStringLiteral("{\"notes\":[]}"));
    bool threadSeen = false;
    for (const QJsonObject &entry : readLog(log)) {
        const QString method = entry.value("method").toString();
        QVERIFY(method != "account/login/start" && method != "account/logout" && !method.startsWith("mcpServer"));
        if (method == "mock/startup") {
            QVERIFY(entry.value("directoryEmpty").toBool());
            QVERIFY(entry.value("cwd").toString() != QDir::currentPath());
            const QStringList args = entry.value("arguments").toVariant().toStringList();
            QVERIFY(args.contains("features.shell_tool=false"));
            QVERIFY(args.contains("features.hooks=false"));
            QVERIFY(args.contains("features.plugins=false"));
        } else if (method == "thread/start") {
            threadSeen = true;
            const QJsonObject parameters = entry.value("params").toObject();
            QVERIFY(parameters.value("ephemeral").toBool());
            QCOMPARE(parameters.value("sandbox").toString(), QStringLiteral("read-only"));
            QCOMPARE(parameters.value("approvalPolicy").toString(), QStringLiteral("never"));
            const QJsonObject mcp = parameters.value("config").toObject().value("mcp_servers").toObject();
            QCOMPARE(mcp.value("test.server").toObject(), QJsonObject({{"enabled", false}}));
            QVERIFY(!QJsonDocument(parameters).toJson().contains("test-only-secret"));
        }
    }
    QVERIFY(threadSeen);
}

void AiTextTransportTest::codexFailures_data() {
    QTest::addColumn<QString>("mode");
    for (const char *mode : {"toolRequest", "toolItem", "unsafeConfig", "unsafeThread", "unsafeNetwork", "failedTurn", "multipleFinal", "loggedOut", "badVersion", "permanentError"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}
void AiTextTransportTest::codexFailures() {
    QFETCH(QString, mode);
    QTemporaryDir directory;
    const QString log = directory.filePath("failure.jsonl");
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(codexPreferences(mode, log));
    QSignalSpy completed(&transport, &lmsc::AiTextTransport::completed);
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    QSignalSpy details(&transport,&lmsc::AiTextTransport::failureInfo);
    transport.complete(request());
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(completed.count(), 0);
    QVERIFY(!failed.first().at(1).toString().contains("test-only-secret"));
    QCOMPARE(details.count(),1);
    if (mode=="permanentError") {
        const auto error=qvariant_cast<lmsc::AiFailure>(details.first().first());
        QCOMPARE(error.category,QString("network")); QVERIFY(error.retryable); QVERIFY(!error.stage.isEmpty());
    }
    if (mode == "unsafeConfig" || mode == "unsafeThread" || mode == "unsafeNetwork" || mode == "loggedOut" || mode == "badVersion")
        QVERIFY(!loggedMethod(log, "turn/start"));
    if (mode.startsWith("tool")) QTRY_VERIFY(loggedMethod(log, "turn/interrupt"));
}

void AiTextTransportTest::codexCancelAndTimeout() {
    QTemporaryDir directory;
    const QString log = directory.filePath("cancel.jsonl");
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(codexPreferences("pause", log));
    QSignalSpy completed(&transport, &lmsc::AiTextTransport::completed);
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    transport.complete(request("cancel"));
    QTRY_VERIFY(loggedMethod(log, "turn/start"));
    transport.cancel("cancel");
    QTRY_VERIFY(loggedMethod(log, "turn/interrupt"));
    QTest::qWait(300);
    QCOMPARE(completed.count(), 0);
    QCOMPARE(failed.count(), 0);
    const QString nextLog = directory.filePath("timeout.jsonl");
    transport.configure(codexPreferences("pause", nextLog));
    auto input = request("timeout");
    input.timeoutMs = 500;
    transport.complete(input);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("timeout"));
    QCOMPARE(completed.count(), 0);
    QTRY_VERIFY(loggedMethod(nextLog, "turn/interrupt"));
}

void AiTextTransportTest::codexNpmWrapperUsesNestedBinary() {
#ifndef Q_OS_WIN
    QSKIP("Windows npm native binary layout");
#else
    QTemporaryDir directory;
    const QString current = directory.filePath("node_modules/@openai/codex/node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/bin/codex.exe");
    const QString stale = directory.filePath("node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/codex/codex.exe");
    QVERIFY(QDir().mkpath(QFileInfo(current).absolutePath()));
    QVERIFY(QDir().mkpath(QFileInfo(stale).absolutePath()));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), current));
    QFile old(stale);
    QVERIFY(old.open(QIODevice::WriteOnly));
    old.write("stale binary must not run");
    old.close();
    const QString wrapper = directory.filePath("codex.cmd");
    QFile shim(wrapper);
    QVERIFY(shim.open(QIODevice::WriteOnly));
    shim.close();
    auto preferences = codexPreferences("success", directory.filePath("wrapper.jsonl"));
    preferences.codexExecutable = wrapper;
    lmsc::ConfiguredAiTextTransport transport;
    transport.configure(preferences);
    QSignalSpy completed(&transport, &lmsc::AiTextTransport::completed);
    QSignalSpy failed(&transport, &lmsc::AiTextTransport::failed);
    transport.complete(request());
    QTRY_VERIFY(completed.count() || failed.count());
    QCOMPARE(failed.count(), 0);
    QCOMPARE(completed.count(), 1);
#endif
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains("--version")) {
        const QByteArray version = qEnvironmentVariable("LMSC_TEXT_TEST_MODE") == "badVersion"
                ? QByteArray("codex-cli 9.9.9\n") : QByteArray("codex-cli 0.147.0\n");
        fwrite(version.constData(), 1, static_cast<size_t>(version.size()), stdout);
        return 0;
    }
    if (application.arguments().contains("app-server")) return mockServer();
    AiTextTransportTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "AiTextTransportTest.moc"
