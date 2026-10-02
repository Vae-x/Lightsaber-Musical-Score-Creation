#include "AiRecognitionPage.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>

namespace lmsc {
namespace {
QLabel *label(const QString &text, QWidget *parent, const char *role = "muted") {
    auto widget = new QLabel(text, parent);
    widget->setTextFormat(Qt::PlainText);
    widget->setWordWrap(true);
    widget->setProperty("role", role);
    return widget;
}

QFrame *card(QWidget *parent) {
    auto widget = new QFrame(parent);
    widget->setProperty("role", "settingsCard");
    auto layout = new QVBoxLayout(widget);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    return widget;
}
}

AiRecognitionPage::AiRecognitionPage(QWidget *parent)
    : QWidget(parent), m_unavailable(new UnavailableAiRecognitionService(this)) {
    setObjectName(QStringLiteral("aiRecognitionPage"));
    setProperty("workspaceSurface", true);
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("role", "workspacePage");
    auto outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto content = new QWidget(scroll);
    content->setProperty("role", "workspacePage");
    auto layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(16);
    layout->addWidget(label(tr("AI 识别"), content, "pageTitle"));
    layout->addWidget(label(tr("分析当前歌曲，为人工编谱提供参考。"), content));

    auto song = card(content);
    auto songLayout = qobject_cast<QVBoxLayout *>(song->layout());
    songLayout->addWidget(label(tr("当前歌曲"), song, "cardTitle"));
    m_songTitle = label(tr("尚未打开歌曲"), song, "cardTitle");
    m_songTitle->setObjectName(QStringLiteral("aiSongTitle"));
    songLayout->addWidget(m_songTitle);
    m_songDetails = label({}, song);
    songLayout->addWidget(m_songDetails);
    layout->addWidget(song);

    auto service = card(content);
    auto serviceLayout = qobject_cast<QVBoxLayout *>(service->layout());
    serviceLayout->addWidget(label(tr("识别服务"), service, "cardTitle"));
    m_serviceStatus = label({}, service);
    m_serviceStatus->setObjectName(QStringLiteral("aiServiceStatus"));
    serviceLayout->addWidget(m_serviceStatus);
    serviceLayout->addWidget(label(tr("本地节拍估计可在“曲谱编辑”中使用。模型连接和账号可在左侧“大语言模型”和“账号授权”中配置。"), service));
    auto actions = new QHBoxLayout;
    actions->setSpacing(10);
    m_start = new QPushButton(tr("开始识别"), service);
    m_start->setObjectName(QStringLiteral("aiRecognizeButton"));
    m_start->setProperty("role", "primary");
    actions->addWidget(m_start);
    m_cancel = new QPushButton(tr("取消识别"), service);
    m_cancel->setObjectName(QStringLiteral("aiCancelRecognition"));
    actions->addWidget(m_cancel);
    actions->addStretch();
    m_configure = new QPushButton(tr("配置 AI 连接"), service);
    m_configure->setObjectName(QStringLiteral("aiConfigureConnection"));
    actions->addWidget(m_configure);
    serviceLayout->addLayout(actions);
    m_status = label({}, service);
    m_status->setObjectName(QStringLiteral("aiRecognitionStatus"));
    serviceLayout->addWidget(m_status);
    m_progress = new QProgressBar(service);
    m_progress->setObjectName(QStringLiteral("aiRecognitionProgress"));
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);
    serviceLayout->addWidget(m_progress);
    layout->addWidget(service);

    auto result = card(content);
    auto resultLayout = qobject_cast<QVBoxLayout *>(result->layout());
    resultLayout->addWidget(label(tr("识别结果"), result, "cardTitle"));
    resultLayout->addWidget(label(tr("识别结果仅供参考；确认后请在曲谱编辑中手动修改。"), result));
    m_result = new QPlainTextEdit(result);
    m_result->setObjectName(QStringLiteral("aiRecognitionResult"));
    m_result->setReadOnly(true);
    m_result->setPlaceholderText(tr("尚无识别结果"));
    m_result->setMinimumHeight(150);
    resultLayout->addWidget(m_result);
    layout->addWidget(result);
    layout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);

    connect(m_start, &QPushButton::clicked, this, &AiRecognitionPage::startRecognition);
    connect(m_cancel, &QPushButton::clicked, this, &AiRecognitionPage::cancelRecognition);
    connect(m_configure, &QPushButton::clicked, this, &AiRecognitionPage::configureConnectionRequested);
    setService(nullptr);
    setContext({}, {}, 120.0, 0.0, 0.0, false);
}

AiRecognitionPage::~AiRecognitionPage() {
    if (!m_pendingContextId.isEmpty()) emit cancelRequested(m_pendingContextId);
    for (const auto &connection : m_connections) disconnect(connection);
}

void AiRecognitionPage::setContext(const QString &audioFile, const QString &title, double bpm,
                                   double offsetSeconds, double durationSeconds, bool busy) {
    const bool changed = m_audioFile != audioFile || m_title != title || m_bpm != bpm
        || m_offsetSeconds != offsetSeconds || m_durationSeconds != durationSeconds;
    if (changed || busy) cancelRecognition();
    if (changed) {
        ++m_sourceRevision;
        m_result->clear();
    }
    const bool busyChanged = m_contextBusy != busy;
    m_audioFile = audioFile;
    m_title = title;
    m_bpm = bpm;
    m_offsetSeconds = offsetSeconds;
    m_durationSeconds = durationSeconds;
    m_contextBusy = busy;
    m_songTitle->setText(!title.isEmpty() ? title : audioFile.isEmpty()
        ? tr("尚未打开歌曲") : QFileInfo(audioFile).completeBaseName());
    if (title.isEmpty() && audioFile.isEmpty()) {
        m_songDetails->setText(tr("先打开已有工程或导入歌曲，再进行识别。"));
    } else {
        const QString duration = std::isfinite(durationSeconds) && durationSeconds > 0.0
            ? tr("%1 秒").arg(durationSeconds, 0, 'f', 1) : tr("时长待确认");
        m_songDetails->setText(tr("%1 BPM · 偏移 %2 秒 · %3")
            .arg(bpm, 0, 'f', 2).arg(offsetSeconds, 0, 'f', 3).arg(duration));
    }
    refreshControls();
    if (changed || busyChanged || m_status->text().isEmpty()) showIdleStatus();
}

void AiRecognitionPage::setService(AiRecognitionService *service) {
    auto next = service ? service : m_unavailable;
    if (m_service == next) return;
    cancelRecognition();
    for (const auto &connection : m_connections) disconnect(connection);
    m_connections.clear();
    m_service = next;
    const auto serviceRevision = ++m_serviceRevision;
    m_result->clear();
    m_connections.append(connect(this, &AiRecognitionPage::analyzeRequested,
                                 next, &AiRecognitionService::analyze));
    m_connections.append(connect(this, &AiRecognitionPage::cancelRequested,
                                 next, &AiRecognitionService::cancel));
    m_connections.append(connect(next, &AiRecognitionService::recognitionFinished, this,
        [this](const AiRecognitionResult &result) {
            if (!accepts(result.contextId)) return;
            m_pendingContextId.clear();
            QString output = result.summary;
            if (!result.events.isEmpty()) {
                if (!output.isEmpty()) output += QStringLiteral("\n\n");
                output += tr("时间参考（秒）");
                for (const auto &event : result.events) {
                    if (!std::isfinite(event.seconds) || event.seconds < 0.0) continue;
                    output += tr("\n%1 · %2").arg(event.seconds, 0, 'f', 3).arg(event.label);
                    if (std::isfinite(event.confidence) && event.confidence > 0.0)
                        output += tr(" · 置信度 %1%").arg(qBound(0.0, event.confidence, 1.0) * 100.0, 0, 'f', 0);
                }
            }
            m_result->setPlainText(output.isEmpty() ? tr("识别完成，未返回可展示的建议。") : output);
            setStatus(tr("识别完成，请在曲谱编辑中人工确认。"), "success");
            refreshControls();
        }));
    m_connections.append(connect(next, &AiRecognitionService::requestFailed, this,
        [this](const QString &contextId, const QString &message) {
            if (!accepts(contextId)) return;
            m_pendingContextId.clear();
            setStatus(message.isEmpty() ? tr("识别失败，请检查识别服务。") : message, "error");
            refreshControls();
        }));
    m_connections.append(connect(next, &AiRecognitionService::progress, this,
        [this](const QString &contextId, int percent) {
            if (!accepts(contextId)) return;
            m_progress->setRange(0, 100);
            m_progress->setValue(qBound(0, percent, 100));
        }));
    m_connections.append(connect(next, &AiRecognitionService::cancelled, this,
        [this](const QString &contextId) {
            if (!accepts(contextId)) return;
            m_pendingContextId.clear();
            setStatus(tr("识别已取消。"));
            refreshControls();
        }));
    m_connections.append(connect(next, &AiRecognitionService::availabilityChanged, this, [this, serviceRevision] {
        if (serviceRevision != m_serviceRevision) return;
        if (!m_service || !m_service->isAvailable()) cancelRecognition();
        refreshControls();
        if (!isRecognizing()) showIdleStatus();
    }));
    m_connections.append(connect(next, &QObject::destroyed, this, [this, serviceRevision] {
        if (serviceRevision != m_serviceRevision) return;
        m_pendingContextId.clear();
        m_service.clear();
        setService(nullptr);
        setStatus(tr("识别服务已断开，AI 音频识别尚未接入。"), "warning");
    }));
    refreshControls();
    showIdleStatus();
}

void AiRecognitionPage::startRecognition() {
    refreshControls();
    if (!m_start->isEnabled()) return;
    m_pendingContextId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    AiRecognitionRequest request;
    request.contextId = m_pendingContextId;
    request.sourceRevision = m_sourceRevision;
    request.audioFile = m_audioFile;
    request.bpm = m_bpm;
    request.offsetSeconds = m_offsetSeconds;
    request.durationSeconds = m_durationSeconds;
    m_result->clear();
    m_progress->setRange(0, 0);
    setStatus(tr("正在识别当前歌曲…"), "status");
    refreshControls();
    emit analyzeRequested(request);
}

void AiRecognitionPage::cancelRecognition() {
    if (m_pendingContextId.isEmpty()) return;
    const auto contextId = m_pendingContextId;
    m_pendingContextId.clear();
    setStatus(tr("识别已取消。"));
    refreshControls();
    emit cancelRequested(contextId);
}

bool AiRecognitionPage::accepts(const QString &contextId) const {
    return !m_pendingContextId.isEmpty() && contextId == m_pendingContextId;
}

void AiRecognitionPage::refreshControls() {
    const bool available = m_service && m_service->isAvailable();
    const auto file = QFileInfo(m_audioFile);
    const bool hasAudio = !m_audioFile.isEmpty() && file.isFile() && file.isReadable();
    const bool validTiming = std::isfinite(m_bpm) && m_bpm > 0.0
        && std::isfinite(m_offsetSeconds) && std::isfinite(m_durationSeconds);
    m_start->setEnabled(available && hasAudio && validTiming && !m_contextBusy && !isRecognizing());
    m_cancel->setEnabled(isRecognizing());
    m_cancel->setVisible(isRecognizing());
    m_progress->setVisible(isRecognizing());
    m_configure->setEnabled(!isRecognizing() && !m_contextBusy);
    m_serviceStatus->setText(available
        ? tr("识别服务已接入")
        : tr("AI 音频识别尚未接入，当前可先配置 AI 连接。"));
    if (!available) m_start->setToolTip(tr("需要先接入支持音频分析的 AI 识别服务。"));
    else if (!hasAudio) m_start->setToolTip(tr("请打开带有可用本地音频的歌曲或工程。"));
    else if (m_contextBusy) m_start->setToolTip(tr("请等待当前任务结束。"));
    else if (!validTiming) m_start->setToolTip(tr("请先确认歌曲的 BPM 和时间参数。"));
    else m_start->setToolTip({});
}

void AiRecognitionPage::showIdleStatus() {
    if (isRecognizing()) return;
    if (!m_service || !m_service->isAvailable()) {
        setStatus(tr("尚未接入识别服务"));
    } else if (m_contextBusy) {
        setStatus(tr("请等待当前任务结束。"));
    } else if (m_audioFile.isEmpty() || !QFileInfo(m_audioFile).isFile()
               || !QFileInfo(m_audioFile).isReadable()) {
        setStatus(tr("请先打开带有可用本地音频的歌曲或工程。"));
    } else if (!std::isfinite(m_bpm) || m_bpm <= 0.0 || !std::isfinite(m_offsetSeconds)
               || !std::isfinite(m_durationSeconds)) {
        setStatus(tr("请先确认歌曲的 BPM 和时间参数。"), "warning");
    } else {
        setStatus(tr("准备就绪，可开始识别。"));
    }
}

void AiRecognitionPage::setStatus(const QString &text, const char *role) {
    m_status->setText(text);
    m_status->setProperty("role", role);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->update();
}

} // namespace lmsc
