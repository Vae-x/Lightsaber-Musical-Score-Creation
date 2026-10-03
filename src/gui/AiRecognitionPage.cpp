#include "AiRecognitionPage.h"
#include "DiagnosticLogDialog.h"

#include <QFileInfo>
#include <QCheckBox>
#include <QComboBox>
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
#include <QSignalBlocker>
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

bool sameTiming(const TimeMap &a, const TimeMap &b) {
    if (a.baseBpm() != b.baseBpm() || a.firstBeatSeconds() != b.firstBeatSeconds()
        || a.changes().size() != b.changes().size()) return false;
    for (int i = 0; i < a.changes().size(); ++i)
        if (a.changes()[i].beat != b.changes()[i].beat || a.changes()[i].bpm != b.changes()[i].bpm) return false;
    return true;
}
}

AiRecognitionPage::AiRecognitionPage(QWidget *parent)
    : QWidget(parent), m_unavailable(new UnavailableAiRecognitionService(this)) {
    qRegisterMetaType<GenerationRequest>();
    qRegisterMetaType<GenerationDraft>();
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
    layout->addWidget(label(tr("AI 分析与制谱"), content, "pageTitle"));
    layout->addWidget(label(tr("分析整首音乐，按难度编排动作；先预览，确认后应用到新歌。"), content));

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
    serviceLayout->addWidget(label(tr("音乐分析与编排"), service, "cardTitle"));
    m_serviceStatus = label({}, service);
    m_serviceStatus->setObjectName(QStringLiteral("aiServiceStatus"));
    serviceLayout->addWidget(m_serviceStatus);
    serviceLayout->addWidget(label(tr("本地快速制谱在电脑上分析音乐并编排动作，无需模型连接或独立显卡。也可选择大语言模型制谱。分析可用于已有歌曲；自动制谱仅支持新建歌曲。"), service));
    auto modes = new QHBoxLayout;
    modes->addWidget(label(tr("生成方式"), service));
    m_mode = new QComboBox(service);
    m_mode->setObjectName(QStringLiteral("aiGenerationMode"));
    m_mode->addItem(tr("本地快速制谱"), LocalQuick);
    m_mode->addItem(tr("大语言模型制谱"), LanguageModel);
    modes->addWidget(m_mode);
    modes->addStretch();
    serviceLayout->addLayout(modes);
    auto options = new QHBoxLayout;
    options->addWidget(label(tr("生成难度"), service));
    m_difficulty = new QComboBox(service);
    m_difficulty->setObjectName(QStringLiteral("aiGenerationDifficulty"));
    const QStringList names{QStringLiteral("Easy"), QStringLiteral("Normal"), QStringLiteral("Hard"),
                            QStringLiteral("Expert"), QStringLiteral("ExpertPlus")};
    const QStringList labels{tr("简单 · Easy"), tr("普通 · Normal"), tr("困难 · Hard"),
                             tr("专家 · Expert"), tr("专家+ · ExpertPlus")};
    for (int i = 0; i < names.size(); ++i) m_difficulty->addItem(labels[i], names[i]);
    m_difficulty->setCurrentIndex(3);
    options->addWidget(m_difficulty);
    options->addStretch();
    serviceLayout->addLayout(options);
    auto types = new QHBoxLayout;
    m_directional = new QCheckBox(tr("方向方块"), service);
    m_dots = new QCheckBox(tr("无方向方块"), service);
    m_bombs = new QCheckBox(tr("炸弹"), service);
    m_walls = new QCheckBox(tr("墙"), service);
    m_directional->setObjectName(QStringLiteral("aiDirectionalType"));
    m_dots->setObjectName(QStringLiteral("aiDotType"));
    m_bombs->setObjectName(QStringLiteral("aiBombType"));
    m_walls->setObjectName(QStringLiteral("aiWallType"));
    m_directional->setChecked(true);
    for (auto box : {m_directional, m_dots, m_bombs, m_walls}) types->addWidget(box);
    types->addStretch();
    serviceLayout->addLayout(types);
    serviceLayout->addWidget(label(tr("勾选要使用的类型；仅选择炸弹或墙也可生成避障练习。合适位置不足时会减少相应物件。"), service));
    auto actions = new QHBoxLayout;
    actions->setSpacing(10);
    m_start = new QPushButton(tr("分析音乐"), service);
    m_start->setObjectName(QStringLiteral("aiRecognizeButton"));
    m_start->setProperty("role", "primary");
    actions->addWidget(m_start);
    m_generate = new QPushButton(tr("生成候选谱"), service);
    m_generate->setObjectName(QStringLiteral("aiGenerateButton"));
    m_generate->setProperty("role", "primary");
    actions->addWidget(m_generate);
    m_cancel = new QPushButton(tr("取消任务"), service);
    m_cancel->setObjectName(QStringLiteral("aiCancelRecognition"));
    actions->addWidget(m_cancel);
    m_resume=new QPushButton(tr("继续生成"), service); m_resume->setObjectName("aiResumeGeneration"); actions->addWidget(m_resume);
    m_preview=new QPushButton(tr("查看候选谱"), service); m_preview->setObjectName("aiViewCandidate"); actions->addWidget(m_preview);
    m_logs=new QPushButton(tr("查看本次日志"), service); m_logs->setObjectName("aiViewLog"); actions->addWidget(m_logs);
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
    resultLayout->addWidget(label(tr("分析与生成结果"), result, "cardTitle"));
    resultLayout->addWidget(label(tr("分析建议不会改变曲谱。生成结果将在独立预览中展示，点击应用才写入工程，且可一次撤销。"), result));
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
    connect(m_generate, &QPushButton::clicked, this, [this] { startGeneration(false); });
    connect(m_cancel, &QPushButton::clicked, this, &AiRecognitionPage::cancelRecognition);
    connect(m_resume, &QPushButton::clicked, this, [this] {
        if (!m_generationService || !m_resume->isEnabled()) return;
        m_pendingGeneration=m_lastGeneration;
        setStatus(tr("正在继续未完成的请求…"), "status"); refreshControls();
        m_generationService->resume(m_pendingGeneration.jobId);
    });
    connect(m_preview, &QPushButton::clicked, this, [this] { if (m_hasDraft) emit generationDraftReady(m_cachedDraft); });
    connect(m_logs, &QPushButton::clicked, this, [this] { showDiagnosticLog(this,m_lastGeneration.jobId); });
    connect(m_configure, &QPushButton::clicked, this, &AiRecognitionPage::configureConnectionRequested);
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const auto next = static_cast<GenerationMode>(m_mode->currentData().toInt());
        if (next == m_generationMode) return;
        m_generationMode = next;
        activateGenerationService(true);
    });
    const auto optionsChanged = [this] {
        if (m_updatingOptions) return;
        invalidateGeneration();
        refreshControls();
        showIdleStatus();
    };
    connect(m_difficulty, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [optionsChanged](int) { optionsChanged(); });
    for (auto box : {m_directional, m_dots, m_bombs, m_walls})
        connect(box, &QCheckBox::toggled, this, [optionsChanged](bool) { optionsChanged(); });
    setService(nullptr);
    setContext({}, {}, 120.0, 0.0, 0.0, false);
}

AiRecognitionPage::~AiRecognitionPage() {
    if (!m_pendingContextId.isEmpty()) emit cancelRequested(m_pendingContextId);
    if (!m_pendingGeneration.jobId.isEmpty()) emit cancelGenerationRequested(m_pendingGeneration.jobId);
    if (m_generationService && !m_lastGeneration.jobId.isEmpty()) m_generationService->discard(m_lastGeneration.jobId);
    for (const auto &connection : m_connections) disconnect(connection);
    for (const auto &connection : m_generationConnections) disconnect(connection);
}

void AiRecognitionPage::setContext(const QString &audioFile, const QString &title, double bpm,
                                   double offsetSeconds, double durationSeconds, bool busy) {
    const bool changed = m_audioFile != audioFile || m_title != title || m_bpm != bpm
        || m_offsetSeconds != offsetSeconds || m_durationSeconds != durationSeconds;
    if (changed || (busy && m_legacyOverride)) {
        cancelRecognition();
        invalidateGeneration();
    }
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
        m_songDetails->setText(tr("先创建新歌，或打开带音频的歌曲与工程。"));
    } else {
        const QString duration = std::isfinite(durationSeconds) && durationSeconds > 0.0
            ? tr("%1 秒").arg(durationSeconds, 0, 'f', 1) : tr("时长待确认");
        m_songDetails->setText(tr("%1 BPM · 偏移 %2 秒 · %3")
            .arg(bpm, 0, 'f', 2).arg(offsetSeconds, 0, 'f', 3).arg(duration));
    }
    refreshControls();
    if (changed || (busyChanged && m_lastGeneration.jobId.isEmpty()) || m_status->text().isEmpty()) showIdleStatus();
}

void AiRecognitionPage::setService(AiRecognitionService *service) {
    auto next = service ? service : m_unavailable;
    m_legacyOverride = service != nullptr;
    if (m_service == next) { refreshControls(); showIdleStatus(); return; }
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

void AiRecognitionPage::setGenerationService(AiGenerationService *service, AiGenerationService *fallback) {
    m_generationFallback = fallback;
    m_modelGenerationService = service;
    if (!usesLocalGeneration()) activateGenerationService();
}

void AiRecognitionPage::setLocalGenerationService(AiGenerationService *service, AiGenerationService *fallback) {
    m_localGenerationFallback = fallback;
    m_localGenerationService = service;
    if (usesLocalGeneration()) activateGenerationService();
}

void AiRecognitionPage::setGenerationMode(GenerationMode mode) {
    const int index = m_mode->findData(mode);
    if (index >= 0) m_mode->setCurrentIndex(index);
}

void AiRecognitionPage::activateGenerationService(bool force) {
    auto service = usesLocalGeneration()
        ? (m_localGenerationService ? m_localGenerationService.data() : m_localGenerationFallback.data())
        : (m_modelGenerationService ? m_modelGenerationService.data() : m_generationFallback.data());
    if (!force && m_generationService == service) { refreshControls(); showIdleStatus(); return; }
    invalidateGeneration();
    for (const auto &connection : m_generationConnections) disconnect(connection);
    m_generationConnections.clear();
    m_generationService = service;
    const auto revision = ++m_generationServiceRevision;
    if (service) {
        m_generationConnections.append(connect(this, &AiRecognitionPage::generationRequested,
                                               service, &AiGenerationService::generate));
        m_generationConnections.append(connect(this, &AiRecognitionPage::cancelGenerationRequested,
                                               service, &AiGenerationService::cancel));
        m_generationConnections.append(connect(service, &AiGenerationService::draftReady, this,
            [this, revision](const GenerationDraft &draft) {
                if (revision != m_generationServiceRevision || !acceptsGeneration(draft.source)) return;
                m_pendingGeneration = {};
                m_cachedDraft=draft; m_hasDraft=!draft.source.analysisOnly;
                m_progress->setRange(0,100); m_progress->setValue(100);
                QString output = draft.summary;
                if (!draft.source.analysisOnly) {
                    output += tr("\n\n%1 · 方向 %2 · 无方向 %3 · 炸弹 %4 · 墙 %5\n平均每秒 %6 个音符 · 峰值 %7")
                        .arg(draft.source.profile.name).arg(draft.metrics.directional).arg(draft.metrics.dots)
                        .arg(draft.metrics.bombs).arg(draft.metrics.walls)
                        .arg(draft.metrics.averageNps, 0, 'f', 2).arg(draft.metrics.peakNps, 0, 'f', 2);
                }
                if (!draft.warnings.isEmpty()) output += QStringLiteral("\n\n") + draft.warnings.join(QStringLiteral("\n"));
                m_result->setPlainText(output.isEmpty() ? tr("分析完成，未返回可展示的建议。") : output);
                setStatus(draft.source.analysisOnly ? tr("分析完成，曲谱未改变。")
                    : draft.hasThemeWarnings ? tr("候选谱已生成，部分乐句建议试听") : tr("候选谱已生成，请试听预览后应用。"),
                    draft.hasThemeWarnings ? "warning" : "success");
                refreshControls();
                if (!draft.source.analysisOnly) emit generationDraftReady(draft);
            }));
        m_generationConnections.append(connect(service, &AiGenerationService::requestFailed, this,
            [this, revision](const QString &jobId, const QString &message) {
                if (revision != m_generationServiceRevision || jobId != m_pendingGeneration.jobId || jobId.isEmpty()) return;
                m_pendingGeneration = {};
                const auto state=m_generationService ? m_generationService->status() : AiGenerationService::Status{};
                const QString retained=state.resumable ? tr("\n已保留 %1/%2 个乐句；处理上述原因后可继续。").arg(state.completedSegments).arg(state.totalSegments) : QString();
                const QString fallback = usesLocalGeneration() ? tr("本地分析或生成失败，请检查音频与时间参数。")
                    : tr("分析或生成失败，请检查模型连接。");
                setStatus((message.isEmpty() ? fallback : message)+retained, "error");
                refreshControls();
            }));
        m_generationConnections.append(connect(service, &AiGenerationService::progress, this,
            [this, revision](const QString &jobId, int percent, const QString &stage) {
                if (revision != m_generationServiceRevision || jobId != m_pendingGeneration.jobId || jobId.isEmpty()) return;
                m_progress->setRange(0, percent < 0 ? 0 : 100);
                if (percent >= 0) m_progress->setValue(qBound(0, percent, 100));
                if (!stage.isEmpty()) setStatus(stage, "status");
                refreshControls();
            }));
        m_generationConnections.append(connect(service, &AiGenerationService::cancelled, this,
            [this, revision](const QString &jobId) {
                if (revision != m_generationServiceRevision || jobId != m_pendingGeneration.jobId || jobId.isEmpty()) return;
                m_pendingGeneration = {};
                setStatus(usesLocalGeneration() ? tr("本地任务已取消，可重新生成。")
                    : tr("任务已停止，已完成进度保留。"));
                refreshControls();
            }));
        m_generationConnections.append(connect(service, &AiGenerationService::availabilityChanged, this,
            [this, revision] {
                if (revision != m_generationServiceRevision) return;
                if (!m_generationService || !m_generationService->isAvailable()) cancelRecognition();
                refreshControls();
                if (!isRecognizing() && m_lastGeneration.jobId.isEmpty()) showIdleStatus();
            }));
        m_generationConnections.append(connect(service, &QObject::destroyed, this,
            [this, revision] {
                if (revision != m_generationServiceRevision) return;
                m_pendingGeneration = {};
                m_generationService.clear();
                activateGenerationService(true);
                setStatus(usesLocalGeneration() ? tr("本地制谱服务已断开。")
                    : tr("生成服务已断开，请检查模型连接。"), "warning");
            }));
    }
    refreshControls();
    showIdleStatus();
}

void AiRecognitionPage::setGenerationContext(const GenerationRequest &context, bool newSong, bool busy) {
    const bool changed = context.documentId != m_generationContext.documentId
        || context.difficultyId != m_generationContext.difficultyId
        || context.documentRevision != m_generationContext.documentRevision
        || context.audioRevision != m_generationContext.audioRevision
        || context.audio.path != m_generationContext.audio.path
        || context.audio.sourcePath != m_generationContext.audio.sourcePath
        || context.audio.durationSeconds != m_generationContext.audio.durationSeconds
        || context.audio.revision != m_generationContext.audio.revision
        || context.profile.name != m_generationContext.profile.name || context.profile.rank != m_generationContext.profile.rank
        || !sameTiming(context.timeMap, m_generationContext.timeMap) || newSong != m_newSong;
    if (changed) invalidateGeneration();
    if (context.documentId != m_generationContext.documentId
        || context.profile.name != m_generationContext.profile.name) {
        m_updatingOptions = true;
        const int index = m_difficulty->findData(context.profile.name);
        if (index >= 0) m_difficulty->setCurrentIndex(index);
        m_updatingOptions = false;
    }
    m_generationContext = context;
    m_newSong = newSong;
    m_contextBusy = busy;
    refreshControls();
    if (changed) showIdleStatus();
}

GeneratedTypes AiRecognitionPage::selectedTypes() const {
    GeneratedTypes types;
    if (m_directional->isChecked()) types |= DirectionalType;
    if (m_dots->isChecked()) types |= DotType;
    if (m_bombs->isChecked()) types |= BombType;
    if (m_walls->isChecked()) types |= WallType;
    return types;
}

bool AiRecognitionPage::acceptsGeneration(const GenerationRequest &source) const {
    const auto &expected = m_pendingGeneration;
    return !expected.jobId.isEmpty() && source.jobId == expected.jobId
        && source.documentId == expected.documentId && source.difficultyId == expected.difficultyId
        && source.documentRevision == expected.documentRevision && source.audioRevision == expected.audioRevision
        && source.audio.path == expected.audio.path && source.audio.sourcePath == expected.audio.sourcePath
        && source.audio.revision == expected.audio.revision && source.audio.durationSeconds == expected.audio.durationSeconds
        && source.audio.sampleRate == expected.audio.sampleRate && source.audio.channels == expected.audio.channels
        && sameTiming(source.timeMap, expected.timeMap) && source.profile.name == expected.profile.name
        && source.profile.rank == expected.profile.rank
        && source.profile.targetMinNps == expected.profile.targetMinNps && source.profile.targetMaxNps == expected.profile.targetMaxNps
        && source.profile.maxPeakNps == expected.profile.maxPeakNps
        && source.profile.minSameHandGapSeconds == expected.profile.minSameHandGapSeconds
        && source.profile.maxConnectionSpeed == expected.profile.maxConnectionSpeed
        && source.profile.subdivision == expected.profile.subdivision && source.allowedTypes == expected.allowedTypes
        && source.analysisOnly == expected.analysisOnly;
}

void AiRecognitionPage::startGeneration(bool analysisOnly) {
    refreshControls();
    if (analysisOnly ? !m_start->isEnabled() : !m_generate->isEnabled()) return;
    emit generationInvalidated();
    m_pendingGeneration = m_generationContext;
    m_pendingGeneration.jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pendingGeneration.profile = DifficultyProfile::forName(m_difficulty->currentData().toString());
    m_pendingGeneration.allowedTypes = selectedTypes();
    m_pendingGeneration.analysisOnly = analysisOnly;
    m_lastGeneration=m_pendingGeneration; m_cachedDraft={}; m_hasDraft=false;
    m_result->clear();
    m_progress->setRange(0, 0);
    setStatus(analysisOnly ? tr("正在分析整首音乐…") : tr("正在分析音乐并生成候选谱…"), "status");
    refreshControls();
    const auto request = m_pendingGeneration;
    emit generationRequested(request);
}

void AiRecognitionPage::invalidateGeneration() {
    cancelRecognition();
    if (m_generationService && !m_lastGeneration.jobId.isEmpty()) m_generationService->discard(m_lastGeneration.jobId);
    m_lastGeneration={}; m_cachedDraft={}; m_hasDraft=false;
    m_result->clear();
    refreshControls();
    emit generationInvalidated();
}
void AiRecognitionPage::pauseGenerationForConnectionChange() {
    if (!usesLocalGeneration()) cancelRecognition();
}

void AiRecognitionPage::invalidateGenerationForConnectionChange() {
    if (!usesLocalGeneration()) invalidateGeneration();
}

void AiRecognitionPage::showGenerationApplied() {
    setStatus(tr("候选谱已应用，可在曲谱编辑中一次撤销。"), "success");
    refreshControls();
}

void AiRecognitionPage::startRecognition() {
    if (!m_legacyOverride && m_generationService) { startGeneration(true); return; }
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
    if (!isRecognizing()) return;
    const auto contextId = m_pendingContextId;
    const auto jobId = m_pendingGeneration.jobId;
    m_pendingContextId.clear();
    m_pendingGeneration = {};
    setStatus(!contextId.isEmpty() ? tr("识别已取消。") : usesLocalGeneration()
        ? tr("本地任务已取消，可重新生成。") : tr("任务已停止，已完成进度保留。"));
    refreshControls();
    if (!contextId.isEmpty()) emit cancelRequested(contextId);
    if (!jobId.isEmpty()) emit cancelGenerationRequested(jobId);
    refreshControls();
}

bool AiRecognitionPage::accepts(const QString &contextId) const {
    return !m_pendingContextId.isEmpty() && contextId == m_pendingContextId;
}

void AiRecognitionPage::refreshControls() {
    const bool available = m_legacyOverride || !m_generationService
        ? m_service && m_service->isAvailable() : m_generationService->isAvailable();
    const auto file = QFileInfo(m_audioFile);
    const bool hasAudio = !m_audioFile.isEmpty() && file.isFile() && file.isReadable();
    const bool validTiming = std::isfinite(m_bpm) && m_bpm > 0.0
        && std::isfinite(m_offsetSeconds) && std::isfinite(m_durationSeconds);
    const bool hasSnapshot = m_generationContext.audio.isValid() && !m_generationContext.documentId.isEmpty()
        && QFileInfo(m_generationContext.audio.path).isFile();
    const bool legacy = m_legacyOverride || !m_generationService;
    m_start->setEnabled(available && hasAudio && validTiming && (legacy || hasSnapshot)
                        && !m_contextBusy && !isRecognizing());
    m_generate->setEnabled(m_generationService && m_generationService->isAvailable() && hasSnapshot
        && validTiming && m_newSong && selectedTypes() != GeneratedTypes() && !m_contextBusy && !isRecognizing());
    m_difficulty->setEnabled(m_newSong && !m_contextBusy);
    m_mode->setEnabled(!m_contextBusy);
    for (auto box : {m_directional, m_dots, m_bombs, m_walls}) box->setEnabled(m_newSong && !m_contextBusy);
    m_cancel->setEnabled(isRecognizing());
    m_cancel->setVisible(isRecognizing());
    const auto state=m_generationService ? m_generationService->status() : AiGenerationService::Status{};
    const bool canResume=!usesLocalGeneration() && !m_lastGeneration.jobId.isEmpty()
        && state.jobId==m_lastGeneration.jobId && state.resumable;
    m_resume->setVisible(canResume); m_resume->setEnabled(canResume && available && !m_contextBusy && !isRecognizing());
    m_preview->setVisible(m_hasDraft); m_preview->setEnabled(m_hasDraft && !m_contextBusy && !isRecognizing());
    m_logs->setVisible(!m_lastGeneration.jobId.isEmpty());
    m_generate->setText(!m_lastGeneration.jobId.isEmpty() && !m_lastGeneration.analysisOnly ? tr("重新生成") : tr("生成候选谱"));
    m_progress->setVisible(isRecognizing() || !m_lastGeneration.jobId.isEmpty());
    m_configure->setEnabled((usesLocalGeneration() || !isRecognizing()) && !m_contextBusy);
    m_configure->setText(usesLocalGeneration() ? tr("模型连接设置（可选）") : tr("配置 AI 连接"));
    if (m_legacyOverride) {
        m_serviceStatus->setText(available ? tr("已连接音频分析服务") : tr("音频分析服务尚未接入。"));
    } else if (usesLocalGeneration()) {
        m_serviceStatus->setText(available ? tr("本地快速制谱已就绪 · 离线运行，无需配置模型连接")
            : tr("本地制谱服务尚未准备就绪。"));
    } else {
        m_serviceStatus->setText(available ? tr("已连接模型分析与编排服务")
            : tr("请先配置并保存模型连接，或完成账号授权。"));
    }
    const QString unavailableTip = usesLocalGeneration() && !m_legacyOverride
        ? tr("本地制谱服务尚未准备就绪。") : tr("请先配置并保存可用的模型连接。");
    if (!available) m_start->setToolTip(unavailableTip);
    else if (!hasAudio) m_start->setToolTip(tr("请打开带有可用本地音频的歌曲或工程。"));
    else if (m_contextBusy) m_start->setToolTip(tr("请等待当前任务结束。"));
    else if (!validTiming) m_start->setToolTip(tr("请先确认歌曲的 BPM 和时间参数。"));
    else m_start->setToolTip({});
    if (!m_newSong) m_generate->setToolTip(tr("自动制谱首版仅支持新建歌曲；已有歌曲可使用分析音乐。"));
    else if (selectedTypes() == GeneratedTypes()) m_generate->setToolTip(tr("请至少勾选一种物件类型。"));
    else if (!hasSnapshot) m_generate->setToolTip(tr("请等待当前歌曲音频准备完成。"));
    else if (!m_generationService || !m_generationService->isAvailable()) m_generate->setToolTip(unavailableTip);
    else m_generate->setToolTip({});
}

void AiRecognitionPage::showIdleStatus() {
    if (isRecognizing() || !m_lastGeneration.jobId.isEmpty()) return;
    const bool available = m_legacyOverride || !m_generationService
        ? m_service && m_service->isAvailable() : m_generationService->isAvailable();
    if (!available) {
        setStatus(usesLocalGeneration() && !m_legacyOverride ? tr("本地制谱服务尚未准备就绪。")
            : tr("请先配置并保存模型连接。"));
    } else if (m_contextBusy) {
        setStatus(tr("请等待当前任务结束。"));
    } else if (m_audioFile.isEmpty() || !QFileInfo(m_audioFile).isFile()
               || !QFileInfo(m_audioFile).isReadable()) {
        setStatus(tr("请先打开带有可用本地音频的歌曲或工程。"));
    } else if (!std::isfinite(m_bpm) || m_bpm <= 0.0 || !std::isfinite(m_offsetSeconds)
               || !std::isfinite(m_durationSeconds)) {
        setStatus(tr("请先确认歌曲的 BPM 和时间参数。"), "warning");
    } else {
        setStatus(tr("准备就绪，可分析音乐或生成候选谱。"));
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
