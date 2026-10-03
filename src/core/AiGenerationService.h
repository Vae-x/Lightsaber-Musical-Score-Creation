#pragma once

#include "BeatmapDocument.h"
#include "PcmAudioSnapshot.h"
#include <QObject>
#include <QFlags>
#include <memory>

namespace lmsc {
class AiTextTransport;
enum GeneratedType { DirectionalType = 0x1, DotType = 0x2, BombType = 0x4, WallType = 0x8 };
Q_DECLARE_FLAGS(GeneratedTypes, GeneratedType)
struct DifficultyProfile {
    QString name = QStringLiteral("Expert");
    int rank = 7;
    double targetMinNps = 3.0, targetMaxNps = 4.2, maxPeakNps = 6.5;
    double minSameHandGapSeconds = 0.2, maxConnectionSpeed = 6.0;
    int subdivision = 4;
    static DifficultyProfile forName(const QString &name);
};
struct GenerationRequest {
    QString jobId, documentId, difficultyId;
    quint64 documentRevision = 0, audioRevision = 0;
    PcmAudioSnapshot audio;
    TimeMap timeMap;
    DifficultyProfile profile;
    GeneratedTypes allowedTypes = DirectionalType;
    bool analysisOnly = false;
};
struct GenerationMetrics {
    int directional = 0, dots = 0, bombs = 0, walls = 0;
    double averageNps = 0.0, peakNps = 0.0;
};
struct GenerationDraft {
    GenerationRequest source;
    QVector<BeatObject> objects;
    QString summary;
    QStringList warnings;
    bool hasThemeWarnings = false;
    GenerationMetrics metrics;
};
class AiGenerationService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool isAvailable() const = 0;
    struct Status {
        enum State { Idle, Running, Paused, Failed, Completed } state = Idle;
        QString jobId, stage, message;
        int percent = 0, completedSegments = 0, totalSegments = 0;
        bool resumable = false;
        bool recovering = false;
        QString pauseCategory;
    };
    virtual Status status() const { return {}; }
public slots:
    virtual void generate(const lmsc::GenerationRequest &request) = 0;
    virtual void cancel(const QString &jobId) = 0;
    virtual void resume(const QString &jobId) { emit requestFailed(jobId, QStringLiteral("此服务不支持继续生成。")); }
    virtual void discard(const QString &jobId) { cancel(jobId); }
signals:
    void draftReady(const lmsc::GenerationDraft &draft);
    void requestFailed(const QString &jobId, const QString &message);
    void progress(const QString &jobId, int percent, const QString &stage);
    void cancelled(const QString &jobId);
    void availabilityChanged();
};
class LlmAiGenerationService final : public AiGenerationService {
    Q_OBJECT
public:
    explicit LlmAiGenerationService(AiTextTransport *transport, QObject *parent = nullptr);
    ~LlmAiGenerationService() override;
    bool isAvailable() const override;
    Status status() const override;
public slots:
    void generate(const lmsc::GenerationRequest &request) override;
    void cancel(const QString &jobId) override;
    void resume(const QString &jobId) override;
    void discard(const QString &jobId) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
Q_DECLARE_OPERATORS_FOR_FLAGS(lmsc::GeneratedTypes)
Q_DECLARE_METATYPE(lmsc::GenerationRequest)
Q_DECLARE_METATYPE(lmsc::GenerationDraft)
