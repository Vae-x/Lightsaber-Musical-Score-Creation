#include "AppSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace lmsc {
namespace {

constexpr qint64 maximumSettingsSize = 4 * 1024 * 1024;

AppPreferences defaults() {
    AppPreferences preferences;
    for (const auto &preset : AppSettings::providerPresets()) {
        AiProviderConfig config;
        config.baseUrl = preset.baseUrl;
        preferences.providers.insert(preset.id, config);
    }
    return preferences;
}

bool protectKey(const QString &key, QString *protectedKey) {
    protectedKey->clear();
    if (key.isEmpty()) return true;
#ifdef Q_OS_WIN
    QByteArray bytes = key.toUtf8();
    DATA_BLOB input = {static_cast<DWORD>(bytes.size()),
                       reinterpret_cast<BYTE *>(bytes.data())};
    DATA_BLOB output = {};
    const bool success = CryptProtectData(&input, L"Lightsaber Musical Score Creation API key",
                                         nullptr, nullptr, nullptr,
                                         CRYPTPROTECT_UI_FORBIDDEN, &output) != FALSE;
    if (success) {
        *protectedKey = QString::fromLatin1(
                QByteArray(reinterpret_cast<const char *>(output.pbData),
                           static_cast<int>(output.cbData)).toBase64());
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
    }
    bytes.fill('\0');
    return success;
#else
    // Refuse to silently downgrade credential storage to plaintext on a new OS.
    Q_UNUSED(key)
    return false;
#endif
}

bool unprotectKey(const QString &protectedKey, QString *key) {
    key->clear();
    if (protectedKey.isEmpty()) return true;
    const QByteArray encoded = protectedKey.toLatin1();
    static const QRegularExpression base64(QStringLiteral("^[A-Za-z0-9+/]+={0,2}$"));
    if (!base64.match(protectedKey).hasMatch() || encoded.size() % 4 != 0) return false;
    QByteArray bytes = QByteArray::fromBase64(encoded);
    if (bytes.isEmpty() || bytes.toBase64() != encoded) return false;
#ifdef Q_OS_WIN
    DATA_BLOB input = {static_cast<DWORD>(bytes.size()),
                       reinterpret_cast<BYTE *>(bytes.data())};
    DATA_BLOB output = {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    QByteArray plain(reinterpret_cast<const char *>(output.pbData),
                     static_cast<int>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    const QString decoded = QString::fromUtf8(plain);
    const bool valid = decoded.toUtf8() == plain;
    if (valid) *key = decoded;
    plain.fill('\0');
    return valid;
#else
    return false;
#endif
}

bool validChoice(const QString &choice, const QStringList &choices) {
    return choices.contains(choice);
}

} // namespace

AppSettings::AppSettings(const QString &filePath) {
    m_filePath = filePath.isEmpty()
            ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                      .filePath(QStringLiteral("preferences.json"))
            : QFileInfo(filePath).absoluteFilePath();
}

QString AppSettings::filePath() const {
    return m_filePath;
}

QVector<AiProviderPreset> AppSettings::providerPresets() {
    return {
        {QStringLiteral("deepseek"), QStringLiteral("DeepSeek"),
         QStringLiteral("https://api.deepseek.com/v1"),
         QStringLiteral("https://api-docs.deepseek.com/zh-cn/")},
        {QStringLiteral("kimi"), QStringLiteral("Kimi（月之暗面）"),
         QStringLiteral("https://api.moonshot.cn/v1"),
         QStringLiteral("https://platform.kimi.com/docs/api/overview")},
        {QStringLiteral("mimo"), QStringLiteral("MiMo（小米）"),
         QStringLiteral("https://api.xiaomimimo.com/v1"),
         QStringLiteral("https://platform.xiaomimimo.com/")},
        {QStringLiteral("openai"), QStringLiteral("OpenAI"),
         QStringLiteral("https://api.openai.com/v1"),
         QStringLiteral("https://platform.openai.com/docs/api-reference/models")},
        {QStringLiteral("qwen"), QStringLiteral("通义千问（百炼）"),
         QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1"),
         QStringLiteral("https://help.aliyun.com/zh/model-studio/compatibility-of-openai-with-dashscope")},
        {QStringLiteral("custom"), QStringLiteral("自定义（OpenAI 兼容）"), {}, {}}
    };
}

QString AppSettings::validateProxy(const NetworkProxyConfig &proxy) {
    if (proxy.mode == QStringLiteral("system")) return {};
    if (proxy.mode != QStringLiteral("manual"))
        return QStringLiteral("代理模式无效，请选择自动或手动设置。");
    const QString host = proxy.host.trimmed();
    if (host.isEmpty()) return QStringLiteral("请填写手动代理的服务器地址。");
    for (const QChar character : host) {
        if (character.isSpace() || character.unicode() < 32 || character.unicode() == 127
                || QStringLiteral("/\\@?#[]").contains(character))
            return QStringLiteral("代理服务器请仅填写 IP 或域名，不要包含协议、端口、路径或账户信息。");
    }
    const QHostAddress address(host);
    if (host.contains(QLatin1Char(':')) && address.protocol() != QAbstractSocket::IPv6Protocol)
        return QStringLiteral("代理服务器请仅填写 IP 或域名，端口请填写在端口栏。");
    if (address.isNull()) {
        const QByteArray domain = QUrl::toAce(host);
        static const QRegularExpression label(QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$"));
        if (domain.isEmpty() || domain.size() > 253)
            return QStringLiteral("代理服务器的 IP 或域名格式无效。");
        const auto labels = domain.split('.');
        for (int i = 0; i < labels.size(); ++i) {
            // A final dot is valid for a fully qualified DNS name.
            if (i == labels.size() - 1 && labels.at(i).isEmpty() && i > 0) continue;
            if (labels.at(i).size() > 63 || !label.match(QString::fromLatin1(labels.at(i))).hasMatch())
                return QStringLiteral("代理服务器的 IP 或域名格式无效。");
        }
    }
    if (proxy.port < 1 || proxy.port > 65535)
        return QStringLiteral("代理端口必须在 1 到 65535 之间。");
    return {};
}

AppPreferences AppSettings::load(QString *error) const {
    if (error) error->clear();
    AppPreferences preferences = defaults();
    QFile file(m_filePath);
    if (!file.exists()) return preferences;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取应用设置，请检查文件权限。");
        return preferences;
    }
    if (file.size() > maximumSettingsSize) {
        if (error) *error = QStringLiteral("应用设置文件过大，已使用默认设置。");
        return preferences;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("应用设置文件格式损坏，已使用默认设置。");
        return preferences;
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("schemaVersion")).isDouble()
            || root.value(QStringLiteral("schemaVersion")).toDouble() != 1) {
        if (error) *error = QStringLiteral("应用设置文件版本不受支持，已使用默认设置。");
        return preferences;
    }
    QStringList warnings;
    const QString theme = root.value(QStringLiteral("themeMode")).toString();
    if (validChoice(theme, {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")}))
        preferences.themeMode = theme;
    else warnings.append(QStringLiteral("主题设置无效，已使用跟随系统。"));
    const QString connection = root.value(QStringLiteral("aiConnection")).toString();
    if (validChoice(connection, {QStringLiteral("api"), QStringLiteral("codex")}))
        preferences.aiConnection = connection;
    else warnings.append(QStringLiteral("AI 连接方式无效，已使用 API。"));
    preferences.codexExecutable = root.value(QStringLiteral("codexExecutable")).toString();
    preferences.codexModel = root.value(QStringLiteral("codexModel")).toString();
    const QJsonValue proxyValue = root.value(QStringLiteral("networkProxy"));
    if (!proxyValue.isUndefined()) {
        const QJsonObject proxy = proxyValue.toObject();
        NetworkProxyConfig config;
        config.mode = proxy.value(QStringLiteral("mode")).toString();
        config.host = proxy.value(QStringLiteral("host")).toString().trimmed();
        const QJsonValue port = proxy.value(QStringLiteral("port"));
        config.port = port.toInt(8080);
        if (!proxyValue.isObject() || !proxy.value(QStringLiteral("host")).isString()
                || !port.isDouble() || port.toDouble() != config.port
                || !validateProxy(config).isEmpty()) {
            warnings.append(QStringLiteral("网络代理设置无效，已使用系统自动代理。"));
        } else {
            preferences.networkProxy = config;
        }
    }

    if (root.value(QStringLiteral("providers")).isObject()) {
        const QJsonObject providers = root.value(QStringLiteral("providers")).toObject();
        for (auto it = providers.constBegin(); it != providers.constEnd(); ++it) {
            if (!it.value().isObject()) {
                warnings.append(QStringLiteral("部分提供商设置无效，已跳过。"));
                continue;
            }
            AiProviderConfig config = preferences.providers.value(it.key());
            const QJsonObject json = it.value().toObject();
            if (json.value(QStringLiteral("baseUrl")).isString())
                config.baseUrl = json.value(QStringLiteral("baseUrl")).toString();
            config.model = json.value(QStringLiteral("model")).toString();
            const QJsonValue keyValue = json.value(QStringLiteral("apiKeyProtected"));
            const QString scheme = json.value(QStringLiteral("keyProtection")).toString();
            if ((!keyValue.isUndefined() && !keyValue.isString())
                    || (!keyValue.toString().isEmpty() && scheme != QStringLiteral("dpapi-user"))
                    || !unprotectKey(keyValue.toString(), &config.apiKey)) {
                warnings.append(QStringLiteral("有 API Key 无法解密，请在当前 Windows 账户下重新填写。"));
            }
            for (const auto &model : json.value(QStringLiteral("models")).toArray()) {
                if (model.isString() && !model.toString().trimmed().isEmpty())
                    config.models.append(model.toString());
            }
            config.models.removeDuplicates();
            preferences.providers.insert(it.key(), config);
        }
    } else {
        warnings.append(QStringLiteral("提供商设置无效，已使用默认值。"));
    }
    const QString providerId = root.value(QStringLiteral("providerId")).toString();
    if (preferences.providers.contains(providerId)) preferences.providerId = providerId;
    else warnings.append(QStringLiteral("已选择的提供商无效，已使用 DeepSeek。"));
    warnings.removeDuplicates();
    if (error) *error = warnings.join(QStringLiteral("\n"));
    return preferences;
}

bool AppSettings::save(const AppPreferences &preferences, QString *error) const {
    if (error) error->clear();
    if (!validChoice(preferences.themeMode,
                     {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")})
            || !validChoice(preferences.aiConnection,
                            {QStringLiteral("api"), QStringLiteral("codex")})) {
        if (error) *error = QStringLiteral("主题或 AI 连接方式无效，设置未保存。");
        return false;
    }
    const QString proxyError = validateProxy(preferences.networkProxy);
    if (!proxyError.isEmpty()) {
        if (error) *error = proxyError;
        return false;
    }
    QJsonObject providers;
    for (auto it = preferences.providers.constBegin(); it != preferences.providers.constEnd(); ++it) {
        QString protectedKey;
        if (!protectKey(it.value().apiKey, &protectedKey)) {
            if (error) *error = QStringLiteral("无法使用 Windows 账户保护 API Key，设置未保存。");
            return false;
        }
        QJsonObject config;
        config.insert(QStringLiteral("baseUrl"), it.value().baseUrl);
        config.insert(QStringLiteral("model"), it.value().model);
        config.insert(QStringLiteral("models"), QJsonArray::fromStringList(it.value().models));
        config.insert(QStringLiteral("keyProtection"), QStringLiteral("dpapi-user"));
        config.insert(QStringLiteral("apiKeyProtected"), protectedKey);
        providers.insert(it.key(), config);
    }
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("themeMode"), preferences.themeMode);
    root.insert(QStringLiteral("aiConnection"), preferences.aiConnection);
    root.insert(QStringLiteral("providerId"), preferences.providerId);
    root.insert(QStringLiteral("codexExecutable"), preferences.codexExecutable);
    root.insert(QStringLiteral("codexModel"), preferences.codexModel);
    root.insert(QStringLiteral("networkProxy"), QJsonObject{
        {QStringLiteral("mode"), preferences.networkProxy.mode},
        {QStringLiteral("host"), preferences.networkProxy.host.trimmed()},
        {QStringLiteral("port"), preferences.networkProxy.port}});
    root.insert(QStringLiteral("providers"), providers);
    const QByteArray contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (contents.size() > maximumSettingsSize) {
        if (error) *error = QStringLiteral("应用设置过大，设置未保存。");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(m_filePath).absolutePath())) {
        if (error) *error = QStringLiteral("无法创建应用设置目录。");
        return false;
    }
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入应用设置，请检查目录权限。");
        return false;
    }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error) *error = QStringLiteral("保存应用设置失败，原设置已保留。");
        return false;
    }
    return true;
}

} // namespace lmsc
