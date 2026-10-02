#pragma once
#include "DiagnosticLog.h"
#include <QMetaType>
#include <QString>
namespace lmsc {
struct AiFailure {
    QString requestId, jobId, stage, category, message;
    int httpStatus = 0, rpcCode = 0, nativeCode = 0;
    qint64 elapsedMs = 0;
    bool retryable = true;
};
inline QString aiHttpCategory(int status) {
    if (status == 401) return "authentication";
    if (status == 403) return "permission";
    if (status == 429) return "quota";
    if (status == 407) return "proxy";
    if (status == 404 || status == 400 || status == 422) return "parameters";
    return status >= 500 ? "server" : "protocol";
}
inline void logAiFailure(const AiFailure &error) {
    DiagnosticLog::instance().record("request.failed", {{"jobId", error.jobId}, {"requestId", error.requestId},
        {"stage", error.stage}, {"category", error.category}, {"httpStatus", error.httpStatus},
        {"rpcCode", error.rpcCode}, {"nativeCode", error.nativeCode}, {"elapsedMs", double(error.elapsedMs)},
        {"retryable", error.retryable}});
}
}
Q_DECLARE_METATYPE(lmsc::AiFailure)
