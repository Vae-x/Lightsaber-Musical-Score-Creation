#pragma once

#include "AiGenerationService.h"
#include "SongArrangementPlan.h"
#include <memory>

namespace lmsc {
// One whole-song model suggestion followed by wholly local note generation.
class HybridAiGenerationService final : public AiGenerationService {
    Q_OBJECT
public:
    explicit HybridAiGenerationService(AiTextTransport *transport, QObject *parent = nullptr);
    ~HybridAiGenerationService() override;
    bool isAvailable() const override { return true; }
    Status status() const override;
public slots:
    void generate(const lmsc::GenerationRequest &request) override;
    void cancel(const QString &jobId) override;
    void discard(const QString &jobId) override;
    void skipPlanning(const QString &jobId);
signals:
    void planReady(const QString &jobId, const lmsc::SongArrangementPlan &plan);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
