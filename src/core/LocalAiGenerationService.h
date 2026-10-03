#pragma once

#include "AiGenerationService.h"
#include <memory>

namespace lmsc {

// Offline generation; owns neither an editor document nor a model connection.
class LocalAiGenerationService final : public AiGenerationService {
    Q_OBJECT
public:
    explicit LocalAiGenerationService(QObject *parent = nullptr);
    ~LocalAiGenerationService() override;
    bool isAvailable() const override { return true; }
    Status status() const override;
public slots:
    void generate(const lmsc::GenerationRequest &request) override;
    void cancel(const QString &jobId) override;
    void discard(const QString &jobId) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace lmsc
