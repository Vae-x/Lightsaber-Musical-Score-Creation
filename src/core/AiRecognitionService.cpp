#include "AiRecognitionService.h"

#include <QTimer>

namespace lmsc {

AiRecognitionService::AiRecognitionService(QObject *parent) : QObject(parent) {
    qRegisterMetaType<AiRecognitionRequest>();
    qRegisterMetaType<AiRecognitionResult>();
}

UnavailableAiRecognitionService::UnavailableAiRecognitionService(QObject *parent)
    : AiRecognitionService(parent) {}

void UnavailableAiRecognitionService::analyze(const AiRecognitionRequest &request) {
    const auto contextId = request.contextId;
    m_pending.insert(contextId);
    QTimer::singleShot(0, this, [this, contextId] {
        if (!m_pending.remove(contextId)) return;
        emit requestFailed(contextId, tr("AI 音频识别尚未接入。配置模型连接后，仍需接入支持音频分析的识别服务。"));
    });
}

void UnavailableAiRecognitionService::cancel(const QString &contextId) {
    if (m_pending.remove(contextId)) emit cancelled(contextId);
}

} // namespace lmsc
