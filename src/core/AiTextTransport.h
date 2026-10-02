#pragma once

#include "AppSettings.h"
#include "AiDiagnostics.h"
#include <QObject>
#include <QJsonObject>
#include <memory>

namespace lmsc {
struct AiTextRequest {
    QString requestId;
    QString jobId;
    QString systemPrompt;
    QString userPrompt;
    QJsonObject outputSchema;
    int timeoutMs = 600000;
    int maxOutputTokens = 8192;
};
struct AiTextResult {
    QString requestId;
    QString text;
    int inputTokens = -1;
    int outputTokens = -1;
};
class AiTextTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool isAvailable() const = 0;
    virtual void configure(const AppPreferences &preferences) = 0;
    virtual int requestTimeoutMs() const { return 600000; }
    virtual QString connectionIdentity() const { return {}; }
public slots:
    virtual void complete(const lmsc::AiTextRequest &request) = 0;
    virtual void cancel(const QString &requestId) = 0;
signals:
    void completed(const lmsc::AiTextResult &result);
    void failed(const QString &requestId, const QString &message);
    void failureInfo(const lmsc::AiFailure &error);
    void requestProgress(const QString &requestId, const QString &stage, qint64 elapsedMs);
    void availabilityChanged();
};
class ConfiguredAiTextTransport final : public AiTextTransport {
    Q_OBJECT
public:
    explicit ConfiguredAiTextTransport(QObject *parent = nullptr);
    ~ConfiguredAiTextTransport() override;
    bool isAvailable() const override;
    void configure(const AppPreferences &preferences) override;
    int requestTimeoutMs() const override;
    QString connectionIdentity() const override;
public slots:
    void complete(const lmsc::AiTextRequest &request) override;
    void cancel(const QString &requestId) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
Q_DECLARE_METATYPE(lmsc::AiTextRequest)
Q_DECLARE_METATYPE(lmsc::AiTextResult)
