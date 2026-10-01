#include "core/AppSettings.h"
#include "core/ApiModelClient.h"
#ifdef Q_OS_WIN
#include "core/WinHttpModelTransport.h"
#endif

#include <QFile>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

struct Response {
    int status = 200;
    QByteArray body = "{\"data\":[{\"id\":\"zeta\"},{\"id\":\"alpha\"},{\"id\":\"zeta\"}]}";
    QByteArray headers;
    int delayMs = 0;
};

class HttpFixture : public QObject {
public:
    QTcpServer server;
    QList<QByteArray> requests;
    QList<Response> responses;

    explicit HttpFixture(QObject *parent = nullptr) : QObject(parent) {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                socket->setParent(this);
                const auto bytes = QSharedPointer<QByteArray>::create();
                const auto recorded = QSharedPointer<bool>::create(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes, recorded] {
                    bytes->append(socket->readAll());
                    if (*recorded || !bytes->contains("\r\n\r\n")) return;
                    *recorded = true;
                    requests.append(*bytes);
                    const Response response = responses.isEmpty() ? Response{} : responses.takeFirst();
                    if (response.delayMs < 0) return;
                    QTimer::singleShot(response.delayMs, socket, [socket, response] {
                        if (socket->state() == QAbstractSocket::UnconnectedState) return;
                        const QByteArray header = "HTTP/1.1 " + QByteArray::number(response.status)
                                + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                + QByteArray::number(response.body.size()) + "\r\n"
                                + response.headers + "\r\n";
                        socket->write(header + response.body);
                        socket->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }

    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1/v1/").arg(server.serverPort());
    }
};

} // namespace

class AiSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void missingSettingsUseProviderDefaults();
    void encryptedRoundTripAndIndependentProviders();
    void invalidSettingsAndAtomicFailure();
    void corruptedKeyRetainsOtherPreferences();
    void proxySettingsRoundTripAndBackwardCompatibility();
    void invalidProxyConfiguration_data();
    void invalidProxyConfiguration();
    void manualProxyIsUsedAndAutomaticModeRestoresDirectLocalAccess();
    void nativeManualProxyReturnsAnHttpResponse();
    void httpsProxyTunnelDoesNotSendCredentialsBeforeTls();
    void invalidProxyPreventsRequests();
    void modelsUseOpenAiFormatAndAuthorization();
    void mimoUsesItsRequiredHeader();
    void rejectedAddressesAndKeys_data();
    void rejectedAddressesAndKeys();
    void serverErrors_data();
    void serverErrors();
    void cancelDoesNotDeliverResults();
    void requestTimeout();
    void replacementSuppressesOldResponse();
    void redirectDoesNotForwardCredentials();
    void oversizedResponseIsRejected();
    void destroyingClientCancelsPendingRequest();
};

void AiSettingsTest::missingSettingsUseProviderDefaults() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::AppSettings settings(directory.filePath(QStringLiteral("config/preferences.json")));
    QString error = QStringLiteral("old error");
    const auto preferences = settings.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(preferences.themeMode, QStringLiteral("system"));
    QCOMPARE(preferences.providerId, QStringLiteral("deepseek"));
    QCOMPARE(preferences.aiConnection, QStringLiteral("api"));
    QCOMPARE(preferences.networkProxy.mode, QStringLiteral("system"));
    QVERIFY(preferences.networkProxy.host.isEmpty());
    QVERIFY(preferences.providers.contains(QStringLiteral("mimo")));
    QCOMPARE(preferences.providers.value(QStringLiteral("kimi")).baseUrl,
             QStringLiteral("https://api.moonshot.cn/v1"));
    for (const auto &provider : preferences.providers) {
        QVERIFY(provider.model.isEmpty());
        QVERIFY(provider.models.isEmpty());
        QVERIFY(provider.apiKey.isEmpty());
    }
    QVERIFY(!QFile::exists(settings.filePath()));
}

void AiSettingsTest::encryptedRoundTripAndIndependentProviders() {
#ifndef Q_OS_WIN
    QSKIP("DPAPI credential protection is supported on Windows.");
#endif
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::AppSettings settings(directory.filePath(QStringLiteral("nested/preferences.json")));
    auto preferences = settings.load();
    preferences.themeMode = QStringLiteral("dark");
    preferences.aiConnection = QStringLiteral("codex");
    preferences.providerId = QStringLiteral("kimi");
    preferences.codexExecutable = QStringLiteral("C:/Program Files/Codex/codex.exe");
    preferences.codexModel = QStringLiteral("custom-codex-model");
    preferences.providers[QStringLiteral("deepseek")].apiKey = QStringLiteral("test-only-deepseek-key");
    preferences.providers[QStringLiteral("deepseek")].model = QStringLiteral("test-deepseek-model");
    preferences.providers[QStringLiteral("kimi")].apiKey = QStringLiteral("test-only-kimi-key");
    preferences.providers[QStringLiteral("kimi")].model = QStringLiteral("test-kimi-model");
    preferences.providers[QStringLiteral("kimi")].models = QStringList{QStringLiteral("test-kimi-model"),
                                                                     QStringLiteral("test-kimi-other")};
    QString error;
    QVERIFY2(settings.save(preferences, &error), qPrintable(error));
    const QByteArray stored = readFile(settings.filePath());
    QVERIFY(!stored.contains("test-only-deepseek-key"));
    QVERIFY(!stored.contains("test-only-kimi-key"));
    QVERIFY(stored.contains("dpapi-user"));
    auto loaded = settings.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.themeMode, preferences.themeMode);
    QCOMPARE(loaded.aiConnection, preferences.aiConnection);
    QCOMPARE(loaded.providerId, preferences.providerId);
    QCOMPARE(loaded.codexExecutable, preferences.codexExecutable);
    QCOMPARE(loaded.codexModel, preferences.codexModel);
    QCOMPARE(loaded.providers.value(QStringLiteral("kimi")).apiKey,
             preferences.providers.value(QStringLiteral("kimi")).apiKey);
    QCOMPARE(loaded.providers.value(QStringLiteral("kimi")).models,
             preferences.providers.value(QStringLiteral("kimi")).models);
    loaded.providers[QStringLiteral("kimi")].apiKey.clear();
    loaded.providers[QStringLiteral("kimi")].model = QStringLiteral("changed-model");
    QVERIFY2(settings.save(loaded, &error), qPrintable(error));
    loaded = settings.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(loaded.providers.value(QStringLiteral("kimi")).apiKey.isEmpty());
    QCOMPARE(loaded.providers.value(QStringLiteral("deepseek")).apiKey,
             QStringLiteral("test-only-deepseek-key"));
    QCOMPARE(loaded.providers.value(QStringLiteral("deepseek")).model,
             QStringLiteral("test-deepseek-model"));
}

void AiSettingsTest::invalidSettingsAndAtomicFailure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::AppSettings settings(directory.filePath(QStringLiteral("preferences.json")));
    QString error;
    auto preferences = settings.load();
    QVERIFY2(settings.save(preferences, &error), qPrintable(error));
    const QByteArray original = readFile(settings.filePath());
    preferences.themeMode = QStringLiteral("invalid-theme");
    QVERIFY(!settings.save(preferences, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(settings.filePath()), original);
    QVERIFY(writeFile(settings.filePath(), "this is not json"));
    QCOMPARE(settings.load(&error).themeMode, QStringLiteral("system"));
    QVERIFY(!error.isEmpty());
    QVERIFY(writeFile(settings.filePath(), "{\"schemaVersion\":2}"));
    QCOMPARE(settings.load(&error).providerId, QStringLiteral("deepseek"));
    QVERIFY(error.contains(QStringLiteral("版本")));
    lmsc::AppSettings directoryTarget(directory.path());
    QVERIFY(!directoryTarget.save(lmsc::AppPreferences{}, &error));
    QVERIFY(!error.isEmpty());
}

void AiSettingsTest::corruptedKeyRetainsOtherPreferences() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::AppSettings settings(directory.filePath(QStringLiteral("preferences.json")));
    auto preferences = settings.load();
    preferences.themeMode = QStringLiteral("light");
    preferences.providers[QStringLiteral("deepseek")].model = QStringLiteral("preserved-model");
    QString error;
    QVERIFY2(settings.save(preferences, &error), qPrintable(error));
    QJsonObject root = QJsonDocument::fromJson(readFile(settings.filePath())).object();
    QJsonObject providers = root.value(QStringLiteral("providers")).toObject();
    QJsonObject deepseek = providers.value(QStringLiteral("deepseek")).toObject();
    deepseek.insert(QStringLiteral("apiKeyProtected"), QStringLiteral("corrupted!"));
    deepseek.insert(QStringLiteral("apiKey"), QStringLiteral("do-not-load-plaintext"));
    providers.insert(QStringLiteral("deepseek"), deepseek);
    root.insert(QStringLiteral("providers"), providers);
    QVERIFY(writeFile(settings.filePath(), QJsonDocument(root).toJson()));
    preferences = settings.load(&error);
    QVERIFY(!error.isEmpty());
    QCOMPARE(preferences.themeMode, QStringLiteral("light"));
    QCOMPARE(preferences.providers.value(QStringLiteral("deepseek")).model,
             QStringLiteral("preserved-model"));
    QVERIFY(preferences.providers.value(QStringLiteral("deepseek")).apiKey.isEmpty());
    QVERIFY(!error.contains(QStringLiteral("do-not-load-plaintext")));
}

void AiSettingsTest::proxySettingsRoundTripAndBackwardCompatibility() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::AppSettings settings(directory.filePath(QStringLiteral("preferences.json")));
    auto preferences = settings.load();
    preferences.networkProxy = {QStringLiteral("manual"), QStringLiteral(" 127.0.0.1 "), 7890};
    QString error;
    QVERIFY2(settings.save(preferences, &error), qPrintable(error));
    auto loaded = settings.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.networkProxy.mode, QStringLiteral("manual"));
    QCOMPARE(loaded.networkProxy.host, QStringLiteral("127.0.0.1"));
    QCOMPARE(loaded.networkProxy.port, 7890);
    const QByteArray original = readFile(settings.filePath());
    preferences.networkProxy.port = 0;
    QVERIFY(!settings.save(preferences, &error));
    QVERIFY(error.contains(QStringLiteral("端口")));
    QCOMPARE(readFile(settings.filePath()), original);
    QJsonObject root = QJsonDocument::fromJson(original).object();
    root.remove(QStringLiteral("networkProxy"));
    QVERIFY(writeFile(settings.filePath(), QJsonDocument(root).toJson()));
    loaded = settings.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.networkProxy.mode, QStringLiteral("system"));
    root.insert(QStringLiteral("networkProxy"), QJsonObject{
        {QStringLiteral("mode"), QStringLiteral("manual")},
        {QStringLiteral("host"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("port"), 7890.5}});
    QVERIFY(writeFile(settings.filePath(), QJsonDocument(root).toJson()));
    loaded = settings.load(&error);
    QVERIFY(error.contains(QStringLiteral("代理")));
    QCOMPARE(loaded.networkProxy.mode, QStringLiteral("system"));
    QCOMPARE(loaded.providerId, preferences.providerId);
    QVERIFY(lmsc::AppSettings::validateProxy({QStringLiteral("manual"), QStringLiteral("::1"), 1}).isEmpty());
    QVERIFY(lmsc::AppSettings::validateProxy({QStringLiteral("manual"), QStringLiteral("proxy.example.invalid"), 65535}).isEmpty());
    QVERIFY(lmsc::AppSettings::validateProxy({QStringLiteral("system"), {}, 0}).isEmpty());
}

void AiSettingsTest::invalidProxyConfiguration_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("host");
    QTest::addColumn<int>("port");
    QTest::newRow("unknown-mode") << QStringLiteral("unknown") << QStringLiteral("localhost") << 8080;
    QTest::newRow("empty-host") << QStringLiteral("manual") << QString{} << 8080;
    QTest::newRow("scheme") << QStringLiteral("manual") << QStringLiteral("http://localhost") << 8080;
    QTest::newRow("path") << QStringLiteral("manual") << QStringLiteral("localhost/proxy") << 8080;
    QTest::newRow("userinfo") << QStringLiteral("manual") << QStringLiteral("user:secret@localhost") << 8080;
    QTest::newRow("embedded-port") << QStringLiteral("manual") << QStringLiteral("localhost:7890") << 8080;
    QTest::newRow("whitespace") << QStringLiteral("manual") << QStringLiteral("local host") << 8080;
    QTest::newRow("control") << QStringLiteral("manual") << QStringLiteral("local\r\nhost") << 8080;
    QTest::newRow("invalid-dns") << QStringLiteral("manual") << QStringLiteral("-invalid.host") << 8080;
    QTest::newRow("empty-dns-label") << QStringLiteral("manual") << QStringLiteral("invalid..host") << 8080;
    QTest::newRow("zero-port") << QStringLiteral("manual") << QStringLiteral("127.0.0.1") << 0;
    QTest::newRow("negative-port") << QStringLiteral("manual") << QStringLiteral("127.0.0.1") << -1;
    QTest::newRow("too-large-port") << QStringLiteral("manual") << QStringLiteral("127.0.0.1") << 65536;
}

void AiSettingsTest::invalidProxyConfiguration() {
    QFETCH(QString, mode);
    QFETCH(QString, host);
    QFETCH(int, port);
    const QString error = lmsc::AppSettings::validateProxy({mode, host, port});
    QVERIFY(!error.isEmpty());
    QVERIFY(!error.contains(QStringLiteral("secret")));
}

void AiSettingsTest::manualProxyIsUsedAndAutomaticModeRestoresDirectLocalAccess() {
#ifndef Q_OS_WIN
    if (!QSslSocket::supportsSsl()) QSKIP("HTTPS runtime libraries are unavailable.");
#endif
    HttpFixture destination;
    HttpFixture proxy;
    QVERIFY(destination.listen());
    QVERIFY(proxy.listen());
    proxy.responses.append(Response{502, {}, {}, 0});
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.setProxyConfig({QStringLiteral("manual"), QStringLiteral("127.0.0.1"), proxy.server.serverPort()});
    client.fetchModels(QStringLiteral("https://proxy-target.invalid/v1"),
                       QStringLiteral("test-only-proxy-key"), QStringLiteral("custom"));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(destination.requests.count(), 0);
    QVERIFY(!proxy.requests.isEmpty());
    const int proxyRequests = proxy.requests.count();
    QVERIFY(proxy.requests.first().startsWith("CONNECT proxy-target.invalid:443 HTTP/1.1"));
    // Explicit manual mode also keeps local gateways on this computer.
    client.fetchModels(destination.baseUrl(), QStringLiteral("test-only-local-key"), QStringLiteral("custom"));
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(destination.requests.count(), 1);
    QCOMPARE(proxy.requests.count(), proxyRequests);
    client.setProxyConfig({});
    client.fetchModels(destination.baseUrl(), QStringLiteral("test-only-direct-key"), QStringLiteral("custom"));
    QTRY_COMPARE(ready.count(), 2);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(proxy.requests.count(), proxyRequests);
    QCOMPARE(destination.requests.count(), 2);
}

void AiSettingsTest::nativeManualProxyReturnsAnHttpResponse() {
#ifdef Q_OS_WIN
    HttpFixture proxy;
    QVERIFY(proxy.listen());
    lmsc::WinHttpModelTransport transport;
    QSignalSpy reply(&transport, &lmsc::WinHttpModelTransport::replyReady);
    // Exercise the transport directly, with no DNS or public network needed.
    // ApiModelClient still rejects plaintext HTTP for all remote API endpoints.
    transport.fetch(QUrl(QStringLiteral("http://transport-target.invalid/v1/models")),
                    QStringLiteral("test-only-native-proxy-key"), QStringLiteral("custom"), 2000,
                    {QStringLiteral("manual"), QStringLiteral("127.0.0.1"), proxy.server.serverPort()});
    QTRY_COMPARE(reply.count(), 1);
    QCOMPARE(reply.first().at(0).toInt(), 200);
    QCOMPARE(reply.first().at(1).toByteArray(), Response{}.body);
    QVERIFY(reply.first().at(2).toString().isEmpty());
    QCOMPARE(proxy.requests.count(), 1);
    QVERIFY(proxy.requests.first().startsWith("GET http://transport-target.invalid/v1/models HTTP/1.1"));
    QVERIFY(proxy.requests.first().contains("Authorization: Bearer test-only-native-proxy-key\r\n"));
#else
    QSKIP("The native WinHTTP transport is Windows-only.");
#endif
}

void AiSettingsTest::httpsProxyTunnelDoesNotSendCredentialsBeforeTls() {
#ifndef Q_OS_WIN
    if (!QSslSocket::supportsSsl()) QSKIP("HTTPS runtime libraries are unavailable.");
#endif
    HttpFixture proxy;
    QVERIFY(proxy.listen());
    proxy.responses.append(Response{502, {}, {}, 0});
    lmsc::ApiModelClient client;
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.setProxyConfig({QStringLiteral("manual"), QStringLiteral("127.0.0.1"), proxy.server.serverPort()});
    client.fetchModels(QStringLiteral("https://proxy-target.invalid/v1"),
                       QStringLiteral("test-only-tunnel-key"), QStringLiteral("custom"));
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(!proxy.requests.isEmpty());
    QVERIFY(proxy.requests.first().startsWith("CONNECT proxy-target.invalid:443 HTTP/1.1"));
    for (const QByteArray &request : proxy.requests)
        QVERIFY(!request.contains("test-only-tunnel-key"));
    QVERIFY(!failed.first().first().toString().contains(QStringLiteral("test-only-tunnel-key")));
}

void AiSettingsTest::invalidProxyPreventsRequests() {
    HttpFixture destination;
    QVERIFY(destination.listen());
    lmsc::ApiModelClient client;
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.setProxyConfig({QStringLiteral("manual"), QStringLiteral("http://bad-proxy"), 8080});
    client.fetchModels(destination.baseUrl(), QStringLiteral("test-only-key"), QStringLiteral("custom"));
    QCOMPARE(failed.count(), 1);
    QVERIFY(!client.isBusy());
    QTest::qWait(50);
    QCOMPARE(destination.requests.count(), 0);
}

void AiSettingsTest::modelsUseOpenAiFormatAndAuthorization() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("test-only-network-key"), QStringLiteral("custom"));
    QVERIFY(client.isBusy());
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(failed.count(), 0);
    QVERIFY(!client.isBusy());
    QCOMPARE(ready.first().first().toStringList(),
             QStringList({QStringLiteral("alpha"), QStringLiteral("zeta")}));
    QCOMPARE(fixture.requests.size(), 1);
    QVERIFY(fixture.requests.first().startsWith("GET /v1/models HTTP/1.1"));
    QVERIFY(fixture.requests.first().contains("Authorization: Bearer test-only-network-key\r\n"));
    QVERIFY(!fixture.requests.first().toLower().contains("api-key:"));
}

void AiSettingsTest::mimoUsesItsRequiredHeader() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("test-only-mimo-key"), QStringLiteral("mimo"));
    QTRY_COMPARE(ready.count(), 1);
    QVERIFY(fixture.requests.first().toLower().contains("api-key: test-only-mimo-key\r\n"));
    QVERIFY(fixture.requests.first().contains("Authorization: Bearer test-only-mimo-key\r\n"));
}

void AiSettingsTest::rejectedAddressesAndKeys_data() {
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("key");
    QTest::newRow("remote-http") << QStringLiteral("http://example.com/v1") << QStringLiteral("synthetic-key");
    QTest::newRow("userinfo") << QStringLiteral("https://user:secret@example.com/v1") << QStringLiteral("synthetic-key");
    QTest::newRow("query") << QStringLiteral("https://example.com/v1?token=secret") << QStringLiteral("synthetic-key");
    QTest::newRow("fragment") << QStringLiteral("https://example.com/v1#secret") << QStringLiteral("synthetic-key");
    QTest::newRow("relative") << QStringLiteral("example.com/v1") << QStringLiteral("synthetic-key");
    QTest::newRow("empty-key") << QStringLiteral("https://example.com/v1") << QString{};
    QTest::newRow("header-injection") << QStringLiteral("https://example.com/v1") << QStringLiteral("synthetic-key\r\nInjected: true");
}

void AiSettingsTest::rejectedAddressesAndKeys() {
    QFETCH(QString, url);
    QFETCH(QString, key);
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.fetchModels(url, key, QStringLiteral("custom"));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QVERIFY(!client.isBusy());
    const QString message = failed.first().first().toString();
    QVERIFY(!message.contains(QStringLiteral("synthetic-key")));
    QVERIFY(!message.contains(QStringLiteral("secret")));
}

void AiSettingsTest::serverErrors_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QString>("expected");
    QTest::newRow("unauthorized") << 401 << QByteArray("echo test-only-secret-key") << QStringLiteral("401");
    QTest::newRow("forbidden") << 403 << QByteArray("{}") << QStringLiteral("403");
    QTest::newRow("missing-endpoint") << 404 << QByteArray("{}") << QStringLiteral("404");
    QTest::newRow("rate-limit") << 429 << QByteArray("{}") << QStringLiteral("429");
    QTest::newRow("server-error") << 500 << QByteArray("{}") << QStringLiteral("500");
    QTest::newRow("invalid-json") << 200 << QByteArray("<html>test-only-secret-key</html>") << QStringLiteral("格式无效");
    QTest::newRow("wrong-shape") << 200 << QByteArray("{\"models\":[\"alpha\"]}") << QStringLiteral("格式无效");
    QTest::newRow("empty-list") << 200 << QByteArray("{\"data\":[]}") << QStringLiteral("没有返回");
}

void AiSettingsTest::serverErrors() {
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(QString, expected);
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    fixture.responses.append(Response{status, body, {}, 0});
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("test-only-secret-key"), QStringLiteral("custom"));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QVERIFY(!client.isBusy());
    const QString message = failed.first().first().toString();
    QVERIFY2(message.contains(expected), qPrintable(message));
    QVERIFY(!message.contains(QStringLiteral("test-only-secret-key")));
}

void AiSettingsTest::cancelDoesNotDeliverResults() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    Response delayed;
    delayed.delayMs = 200;
    fixture.responses.append(delayed);
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("synthetic-key"), QStringLiteral("custom"));
    QTRY_COMPARE(fixture.requests.count(), 1);
    client.cancel();
    QVERIFY(!client.isBusy());
    QTest::qWait(250);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void AiSettingsTest::requestTimeout() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    Response stalled;
    stalled.delayMs = -1;
    fixture.responses.append(stalled);
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("synthetic-key"), QStringLiteral("custom"), 100);
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("超时")));
    QVERIFY(!client.isBusy());
    QCOMPARE(ready.count(), 0);
    QTest::qWait(150);
    QCOMPARE(failed.count(), 1);
}

void AiSettingsTest::replacementSuppressesOldResponse() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    fixture.responses.append(Response{200, "{\"data\":[{\"id\":\"old-model\"}]}", {}, 250});
    fixture.responses.append(Response{200, "{\"data\":[{\"id\":\"new-model\"}]}", {}, 0});
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("old-synthetic-key"), QStringLiteral("custom"));
    QTRY_COMPARE(fixture.requests.count(), 1);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("new-synthetic-key"), QStringLiteral("custom"));
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(ready.first().first().toStringList(), QStringList{QStringLiteral("new-model")});
    QTest::qWait(300);
    QCOMPARE(ready.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(fixture.requests.count(), 2);
}

void AiSettingsTest::redirectDoesNotForwardCredentials() {
    HttpFixture fixture;
    HttpFixture destination;
    QVERIFY(fixture.listen());
    QVERIFY(destination.listen());
    const QByteArray location = "Location: " + destination.baseUrl().toUtf8() + "models\r\n";
    fixture.responses.append(Response{302, {}, location, 0});
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("synthetic-key"), QStringLiteral("custom"));
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("重定向")));
    QTest::qWait(100);
    QCOMPARE(destination.requests.count(), 0);
    QCOMPARE(ready.count(), 0);
}

void AiSettingsTest::oversizedResponseIsRejected() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    fixture.responses.append(Response{200, QByteArray(4 * 1024 * 1024 + 1, ' '), {}, 0});
    lmsc::ApiModelClient client;
    QSignalSpy ready(&client, &lmsc::ApiModelClient::modelsReady);
    QSignalSpy failed(&client, &lmsc::ApiModelClient::requestFailed);
    client.fetchModels(fixture.baseUrl(), QStringLiteral("synthetic-key"), QStringLiteral("custom"));
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("响应过大")));
    QCOMPARE(ready.count(), 0);
    QVERIFY(!client.isBusy());
}

void AiSettingsTest::destroyingClientCancelsPendingRequest() {
    HttpFixture fixture;
    QVERIFY(fixture.listen());
    fixture.responses.append(Response{200, {}, {}, -1});
    auto *client = new lmsc::ApiModelClient;
    client->fetchModels(fixture.baseUrl(), QStringLiteral("synthetic-key"), QStringLiteral("custom"), 20000);
    QTRY_COMPARE(fixture.requests.count(), 1);
    QElapsedTimer timer;
    timer.start();
    delete client;
    QVERIFY2(timer.elapsed() < 1500, "Destroying the client should wake/cancel its request worker promptly.");
    // Also exercise shutdown before the worker has begun opening native handles.
    for (int i = 0; i < 10; ++i) {
        client = new lmsc::ApiModelClient;
        client->fetchModels(fixture.baseUrl(), QStringLiteral("synthetic-key"), QStringLiteral("custom"));
        delete client;
    }
    QTest::qWait(100);
}

QTEST_GUILESS_MAIN(AiSettingsTest)
#include "AiSettingsTest.moc"
