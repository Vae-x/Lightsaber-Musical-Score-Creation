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
};

struct AppPreferences {
    QString themeMode = QStringLiteral("system");
    QString aiConnection = QStringLiteral("api");
    QString providerId = QStringLiteral("deepseek");
    QMap<QString, AiProviderConfig> providers;
    QString codexExecutable;
    QString codexModel;
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

private:
    QString m_filePath;
};

} // namespace lmsc
