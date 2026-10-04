#pragma once

#include "RefinementTypes.h"
#include <memory>

namespace lmsc {
// Session-only refinements; never writes an editor document or source files.
class AiRefinementService : public QObject {
    Q_OBJECT
public:
    explicit AiRefinementService(AiTextTransport *transport, QObject *parent = nullptr);
    ~AiRefinementService() override;
    virtual bool isAvailable() const;
    virtual AiGenerationService::Status status() const;
public slots:
    virtual void refine(const lmsc::RefinementRequest &request);
    virtual void cancel(const QString &jobId);
    virtual void resume(const QString &jobId);
    virtual void discard(const QString &jobId);
signals:
    void candidateReady(const lmsc::RefinementResult &result);
    void requestFailed(const QString &jobId, const QString &message);
    void progress(const QString &jobId, int percent, const QString &stage);
    void cancelled(const QString &jobId);
    void availabilityChanged();
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
