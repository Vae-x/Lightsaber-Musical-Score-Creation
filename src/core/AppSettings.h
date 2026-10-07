#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lmsc {

struct AiProviderPreset {
    QString id;
    QString name;
    QString baseUrl;
    QString documentationUrl;
};

struct AiProviderConfig {
    QString baseUrl;
    QString apiKey;
    QString model;
    QStringList models;
    // Zero uses the verified model policy, or the service default for unknown models.
    int maxOutputTokens = 0;
};

struct NetworkProxyConfig {
    QString mode = QStringLiteral("system");
    QString host;
    int port = 8080;
};

struct AppPreferences {
    QString themeMode = QStringLiteral("system");
    QString aiConnection = QStringLiteral("api");
    QString providerId = QStringLiteral("deepseek");
    QMap<QString, AiProviderConfig> providers;
    QString codexExecutable;
    QString codexModel;
    QString infernoRuntimeDirectory = QStringLiteral("E:/lmsc-infernosaber-runtime");
    QString infernoModelCacheDirectory = QStringLiteral("E:/lmsc-infernosaber-runtime/models");
    int infernoThreads = 4;
    NetworkProxyConfig networkProxy;
    int requestTimeoutMinutes = 10;
    bool diagnosticLogEnabled = true;
};

// Application-wide preferences are separate from songs and project documents.
// API keys are protected for the current Windows user with DPAPI.
class AppSettings {
public:
    explicit AppSettings(const QString &filePath = {});
    AppPreferences load(QString *error = nullptr) const;
    bool save(const AppPreferences &preferences, QString *error = nullptr) const;
    QString filePath() const;
    static QVector<AiProviderPreset> providerPresets();
    static QString validateProxy(const NetworkProxyConfig &proxy);
    static QString validateInfernoSettings(const AppPreferences &preferences);

private:
    QString m_filePath;
};

} // namespace lmsc
