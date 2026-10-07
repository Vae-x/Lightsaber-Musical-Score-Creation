#pragma once

#include "AiGenerationService.h"

namespace lmsc {

// Offline, isolated inference. No installer, downloads or device operations.
class InfernoSaberGenerationService final : public AiGenerationService {
    Q_OBJECT
public:
    struct Config {
        QString runtimeDirectory = QStringLiteral("E:/lmsc-infernosaber-runtime");
        QString modelCacheDirectory = QStringLiteral("E:/lmsc-infernosaber-runtime/models");
        QString runnerPath;
        QString toolsDirectory;
        QString workDirectory;
        int threads = 4;
        int timeoutSeconds = 600;
    };
    explicit InfernoSaberGenerationService(QObject *parent = nullptr);
    ~InfernoSaberGenerationService() override;
    void setConfig(const Config &config);
    Config config() const;
    QString availabilityReason() const;
    QString jobDirectory(const QString &jobId) const;
    bool isAvailable() const override;
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
