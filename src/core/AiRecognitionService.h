#pragma once

#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

namespace lmsc {

// The context ID identifies one request. A backend must echo it in every signal,
// including terminal signals, so consumers can discard cancelled or stale work.
struct AiRecognitionRequest {
    QString contextId;
    quint64 sourceRevision = 0;
    QString audioFile; // Existing local audio; never an implicit upload request.
    double bpm = 120.0;
    double offsetSeconds = 0.0;
    double durationSeconds = 0.0; // <= 0 means the duration is not yet known.
};

struct AiRecognitionEvent {
    double seconds = 0.0;
    QString label;
    double confidence = 0.0;
};

struct AiRecognitionResult {
    QString contextId;
    QString summary;
    QVector<AiRecognitionEvent> events;
};

// Implementations supply analysis suggestions only. They must not modify an
// editor document or its audio, and must stop emitting results for cancelled IDs.
// Configuring a model connection alone does not constitute audio recognition.
// Keep the service object in the GUI thread; implementations can use workers
// internally. The page reads availability synchronously and dispatches via signals.
class AiRecognitionService : public QObject {
    Q_OBJECT
public:
    explicit AiRecognitionService(QObject *parent = nullptr);
    ~AiRecognitionService() override = default;
    virtual bool isAvailable() const = 0;

public slots:
    virtual void analyze(const lmsc::AiRecognitionRequest &request) = 0;
    virtual void cancel(const QString &contextId) = 0;

signals:
    void recognitionFinished(const lmsc::AiRecognitionResult &result);
    void requestFailed(const QString &contextId, const QString &message);
    void progress(const QString &contextId, int percent);
    void cancelled(const QString &contextId);
    void availabilityChanged();
};

// The shipping backend does not read audio, contact a service, or fabricate a
// result. It keeps the integration point explicit until a real backend is added.
class UnavailableAiRecognitionService final : public AiRecognitionService {
    Q_OBJECT
public:
    explicit UnavailableAiRecognitionService(QObject *parent = nullptr);
    bool isAvailable() const override { return false; }

public slots:
    void analyze(const lmsc::AiRecognitionRequest &request) override;
    void cancel(const QString &contextId) override;

private:
    QSet<QString> m_pending;
};

} // namespace lmsc

Q_DECLARE_METATYPE(lmsc::AiRecognitionRequest)
Q_DECLARE_METATYPE(lmsc::AiRecognitionResult)
