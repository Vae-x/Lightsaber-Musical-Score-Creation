#include "GenerationPreviewDialog.h"
#include "EditorViews.h"
#include "core/AudioService.h"
#include "core/AiRefinementService.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QSignalBlocker>
#include <QUuid>
#include <cmath>

namespace lmsc {
GenerationPreviewDialog::GenerationPreviewDialog(const GenerationDraft &draft, int replacedObjectCount,
                                                 AudioService *audio, QWidget *parent, bool addsDifficulty)
    : QDialog(parent), m_draft(draft), m_audio(audio), m_timeline(new TimelineView(this)),
      m_track(new TrackView(this)), m_status(new QLabel(this)), m_play(new QPushButton(this)) {
    setObjectName(QStringLiteral("generationPreviewDialog"));
    setWindowTitle(tr("候选曲谱预览"));
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1080, 820);
#ifdef Q_OS_ANDROID
    if (parent) resize(parent->size());
#endif
    QSet<QString> ids;
    for (const auto &object : m_draft.objects) if (!object.id.isEmpty()) ids.insert(object.id);
    int nextId = 0;
    for (auto &object : m_draft.objects) if (object.id.isEmpty()) {
        QString id;
        do { id = QStringLiteral("candidate:%1").arg(nextId++); } while (ids.contains(id));
        object.id = id; ids.insert(id);
    }
    m_initialDraft = m_draft;
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    auto title = new QLabel(tr("%1 · 候选曲谱").arg(draft.source.profile.name), this);
    title->setProperty("role", "title");
    layout->addWidget(title);
    auto details = new QScrollArea(this);
    details->setObjectName(QStringLiteral("generationPreviewDetails"));
    details->setFrameShape(QFrame::NoFrame);
    details->setWidgetResizable(true);
    details->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    details->setMinimumHeight(72);
    details->setMaximumHeight(125);
    auto detailsContent = new QWidget(details);
    auto detailsLayout = new QVBoxLayout(detailsContent);
    detailsLayout->setContentsMargins(0, 0, 6, 0);
    detailsLayout->setSpacing(6);
    details->setWidget(detailsContent);
    layout->addWidget(details);
    int directional = 0, dots = 0, bombs = 0, walls = 0;
    QVector<EditorObject> objects;
    for (const auto &source : m_draft.objects) {
        EditorObject object;
        object.id = source.id;
        object.beat = source.beat; object.type = static_cast<int>(source.kind);
        object.x = source.x; object.y = source.y; object.color = source.color; object.direction = source.direction;
        object.duration = source.duration; object.width = source.width; object.height = source.height;
        object.locked = source.isProtected(); object.protectedReason = source.protectedReason;
        objects.append(object);
        if (source.kind == ObjectKind::Bomb) ++bombs;
        else if (source.kind == ObjectKind::Wall) ++walls;
        else if (source.direction == 8) ++dots;
        else ++directional;
    }
    auto counts = new QLabel(tr("方向 %1 · 无方向 %2 · 炸弹 %3 · 墙 %4\n平均每秒 %5 个音符 · 峰值 %6")
        .arg(directional).arg(dots).arg(bombs).arg(walls)
        .arg(draft.metrics.averageNps, 0, 'f', 2).arg(draft.metrics.peakNps, 0, 'f', 2), detailsContent);
    counts->setObjectName(QStringLiteral("generationPreviewCounts"));
    counts->setWordWrap(true);
    m_counts = counts;
    detailsLayout->addWidget(counts);
    const QString application = addsDifficulty
        ? tr("应用后将添加 %1 难度，保留其他难度；全部变化可一次撤销。").arg(draft.source.profile.name)
        : tr("应用后将替换 %1 难度的 %2 个物件，保留其他难度；全部变化可一次撤销。")
            .arg(draft.source.profile.name).arg(replacedObjectCount);
    auto notice = new QLabel(application+tr("\n工程音频和时间保持原样，开场缓冲仍在导出时添加。"), detailsContent);
    notice->setObjectName(QStringLiteral("generationReplaceNotice"));
    notice->setWordWrap(true);
    detailsLayout->addWidget(notice);
    m_warnings = new QLabel(draft.warnings.join(QStringLiteral("\n")), detailsContent);
    m_warnings->setTextFormat(Qt::PlainText);
    m_warnings->setObjectName(QStringLiteral("generationPreviewWarnings"));
    m_warnings->setWordWrap(true);
    m_warnings->setProperty("role", "warning");
    m_warnings->setVisible(!draft.warnings.isEmpty());
    detailsLayout->addWidget(m_warnings);
    m_refinementPanel = new QWidget(this);
    m_refinementPanel->setObjectName(QStringLiteral("generationRefinementPanel"));
    auto refinementLayout = new QVBoxLayout(m_refinementPanel);
    refinementLayout->setContentsMargins(0, 0, 0, 0);
    refinementLayout->setSpacing(6);
    auto explanation = new QLabel(tr("每次精修最多 5 个重点乐句，优先检查重复主题、段落衔接和动作过紧的位置；初稿始终保留，可比较和恢复。"), m_refinementPanel);
    explanation->setWordWrap(true);
    refinementLayout->addWidget(explanation);
#ifdef Q_OS_ANDROID
    auto range = new QVBoxLayout;
#else
    auto range = new QHBoxLayout;
#endif
    m_selectedOnly = new QCheckBox(tr("仅精修选段"), m_refinementPanel);
    m_selectedOnly->setObjectName(QStringLiteral("refinementSelectedOnly"));
    range->addWidget(m_selectedOnly);
    m_rangeStart = new QDoubleSpinBox(m_refinementPanel);
    m_rangeEnd = new QDoubleSpinBox(m_refinementPanel);
    m_rangeStart->setObjectName(QStringLiteral("refinementStartSeconds"));
    m_rangeEnd->setObjectName(QStringLiteral("refinementEndSeconds"));
    for (auto spin : {m_rangeStart, m_rangeEnd}) {
        spin->setRange(0, qMax(0.0, draft.source.audio.durationSeconds));
        spin->setDecimals(3); spin->setSuffix(tr(" 秒"));
    }
    m_rangeEnd->setValue(draft.source.audio.durationSeconds);
    range->addWidget(m_rangeStart); range->addWidget(new QLabel(tr("至"), m_refinementPanel)); range->addWidget(m_rangeEnd);
#ifdef Q_OS_ANDROID
    range->addWidget(new QLabel(tr("时间轴使用“循环”工具拖动也可选段"), m_refinementPanel));
#else
    range->addWidget(new QLabel(tr("Shift 拖动时间轴也可选段"), m_refinementPanel));
#endif
    range->addStretch(); refinementLayout->addLayout(range);
#ifdef Q_OS_ANDROID
    auto refinementActions = new QVBoxLayout;
#else
    auto refinementActions = new QHBoxLayout;
#endif
    m_refine = new QPushButton(tr("AI 精修重点片段"), m_refinementPanel);
    m_refine->setObjectName(QStringLiteral("generationRefineButton"));
    m_resumeRefinement = new QPushButton(tr("继续精修"), m_refinementPanel);
    m_resumeRefinement->setObjectName(QStringLiteral("generationRefinementResume"));
    m_cancelRefinement = new QPushButton(tr("取消精修"), m_refinementPanel);
    m_cancelRefinement->setObjectName(QStringLiteral("generationRefinementCancel"));
    m_comparison = new QComboBox(m_refinementPanel);
    m_comparison->setObjectName(QStringLiteral("generationPreviewComparison"));
    m_comparison->addItem(tr("查看初稿"), false); m_comparison->addItem(tr("查看精修谱"), true);
    m_restore = new QPushButton(tr("恢复初稿"), m_refinementPanel);
    m_restore->setObjectName(QStringLiteral("generationRestoreInitial"));
    for (auto button : {m_refine, m_resumeRefinement, m_cancelRefinement}) refinementActions->addWidget(button);
    refinementActions->addStretch(); refinementActions->addWidget(m_comparison); refinementActions->addWidget(m_restore);
    refinementLayout->addLayout(refinementActions);
    m_refinementStats = new QLabel(m_refinementPanel);
    m_refinementStats->setObjectName(QStringLiteral("generationRefinementStats"));
    m_refinementStats->setWordWrap(true); refinementLayout->addWidget(m_refinementStats);
    m_refinementProgress = new QProgressBar(m_refinementPanel);
    m_refinementProgress->setObjectName(QStringLiteral("generationRefinementProgress"));
    refinementLayout->addWidget(m_refinementProgress);
    auto refinementScroll = new QScrollArea(this);
    refinementScroll->setObjectName(QStringLiteral("generationRefinementScroll"));
    refinementScroll->setFrameShape(QFrame::NoFrame); refinementScroll->setWidgetResizable(true);
    refinementScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    refinementScroll->setMinimumHeight(90); refinementScroll->setMaximumHeight(155);
    refinementScroll->setWidget(m_refinementPanel); m_refinementScroll = refinementScroll;
    layout->addWidget(refinementScroll); refinementScroll->hide();
    m_track->setObjectName(QStringLiteral("generationPreviewTrack"));
    m_timeline->setObjectName(QStringLiteral("generationPreviewTimeline"));
    m_timeline->setReadOnly(true);
    // Preview panes can shrink without changing the full editor's minimums.
#ifdef Q_OS_ANDROID
    m_track->setMinimumSize(0, 100);
    m_timeline->setMinimumSize(0, 100);
#else
    m_track->setMinimumSize(300, 100);
    m_timeline->setMinimumHeight(140);
#endif
    const auto time = draft.source.timeMap;
    const auto toSeconds = [time](double beat) { return time.beatToSeconds(beat); };
    const auto toBeat = [time](double seconds) { return time.secondsToBeat(seconds); };
    m_track->setTimeMapping(toSeconds, toBeat);
    m_timeline->setTimeMapping(toSeconds, toBeat);
    m_track->setObjects(objects);
    m_timeline->setObjects(objects);
    m_timeline->setDuration(draft.source.audio.durationSeconds);
    if (audio) m_timeline->setWaveform(audio->waveform(), draft.source.audio.durationSeconds);
#ifdef Q_OS_ANDROID
    auto split = new QTabWidget(this);
    split->addTab(m_track, tr("预览"));
    auto timelinePage = new QWidget;
    auto timelineLayout = new QVBoxLayout(timelinePage);
    auto tools = new QHBoxLayout;
    auto mode = new QComboBox(timelinePage);
    mode->addItems({tr("定位"), tr("平移"), tr("循环")});
    auto zoomIn = new QPushButton(tr("放大"), timelinePage);
    auto zoomOut = new QPushButton(tr("缩小"), timelinePage);
    tools->addWidget(mode, 1); tools->addWidget(zoomIn); tools->addWidget(zoomOut);
    timelineLayout->addLayout(tools); timelineLayout->addWidget(m_timeline, 1);
    split->addTab(timelinePage, tr("时间轴"));
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), m_timeline, [this](int index) { m_timeline->setInteractionMode(index); });
    connect(zoomIn, &QPushButton::clicked, m_timeline, [this] { m_timeline->zoomBy(1.4); });
    connect(zoomOut, &QPushButton::clicked, m_timeline, [this] { m_timeline->zoomBy(1.0 / 1.4); });
#else
    auto split = new QSplitter(Qt::Vertical, this);
    split->addWidget(m_track);
    split->addWidget(m_timeline);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);
#endif
    layout->addWidget(split, 1);
    auto transport = new QHBoxLayout;
    m_play->setObjectName(QStringLiteral("generationPreviewPlay"));
    m_play->setText(audio && audio->isPlaying() ? tr("暂停") : tr("试听"));
    m_play->setEnabled(audio && audio->isReady());
    transport->addWidget(m_play);
#ifndef Q_OS_ANDROID
    transport->addWidget(new QLabel(tr("点击时间轴跳转 · 滚轮缩放 · 中键拖动平移"), this));
#endif
    transport->addStretch();
    layout->addLayout(transport);
    if (audio) {
        m_initialPosition = audio->position();
        m_initialPlaying = audio->isPlaying();
        m_initialLoopEnabled = audio->loopEnabled();
        m_initialLoopStart = audio->loopStartSeconds();
        m_initialLoopEnd = audio->loopEndSeconds();
        audio->setLoop(0.0, draft.source.audio.durationSeconds, false);
        m_timeline->setPlayheadSeconds(m_initialPosition);
        m_track->setPlayheadSeconds(m_initialPosition);
        connect(audio, &AudioService::positionChanged, this, [this](double seconds) {
            m_timeline->setPlayheadSeconds(seconds); m_track->setPlayheadSeconds(seconds);
        });
        connect(audio, &AudioService::playbackChanged, this, [this](bool playing) {
            m_play->setText(playing ? tr("暂停") : tr("试听"));
        });
        connect(m_play, &QPushButton::clicked, this, [this] {
            if (!m_audio || !m_audio->isReady()) return;
            m_changedPlayback = true;
            if (m_audio->isPlaying()) m_audio->pause(); else m_audio->play();
        });
        connect(m_timeline, &TimelineView::seekRequested, this, [this](double seconds) {
            if (!m_audio || !m_audio->isReady()) return;
            m_changedPlayback = true;
            m_audio->seek(seconds);
        });
    }
    connect(m_track, &TrackView::selectionChanged, m_timeline, &TimelineView::setSelectedIds);
    connect(m_timeline, &TimelineView::selectionChanged, m_track, &TrackView::setSelectedIds);
    m_status->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("generationPreviewStatus"));
    m_status->hide();
    layout->addWidget(m_status);
    connect(m_timeline, &TimelineView::loopChanged, this, &GenerationPreviewDialog::setRefinementSelection);
    connect(m_selectedOnly, &QCheckBox::toggled, this, [this] { refreshRefinementControls(); });
    for (auto spin : {m_rangeStart, m_rangeEnd}) connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
        if (m_selectedOnly->isChecked()) m_timeline->setLoop(m_rangeStart->value(), m_rangeEnd->value());
        refreshRefinementControls();
    });
    connect(m_refine, &QPushButton::clicked, this, &GenerationPreviewDialog::refineChart);
    connect(m_resumeRefinement, &QPushButton::clicked, this, &GenerationPreviewDialog::resumeRefinement);
    connect(m_cancelRefinement, &QPushButton::clicked, this, &GenerationPreviewDialog::cancelRefinement);
    connect(m_restore, &QPushButton::clicked, this, &GenerationPreviewDialog::restoreInitialDraft);
    connect(m_comparison, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        m_showRefinement = m_hasRefinement && m_comparison->currentData().toBool();
        m_draft = m_showRefinement ? m_refinement.candidate : m_initialDraft;
        displayDraft(); refreshRefinementControls();
    });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
#ifdef Q_OS_ANDROID
    buttons->setOrientation(Qt::Vertical);
#endif
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("generationPreviewCancel"));
    auto apply = buttons->addButton(tr("应用候选谱"), QDialogButtonBox::AcceptRole);
    m_apply = apply;
    auto regenerate = buttons->addButton(tr("重新生成"), QDialogButtonBox::ActionRole);
    regenerate->setObjectName(QStringLiteral("generationPreviewRegenerate"));
    connect(regenerate, &QPushButton::clicked, this, &GenerationPreviewDialog::regenerateRequested);
    apply->setObjectName(QStringLiteral("generationPreviewApply"));
    apply->setProperty("role", "primary");
    apply->setEnabled(!draft.objects.isEmpty());
    connect(apply, &QPushButton::clicked, this, &GenerationPreviewDialog::applyRequested);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    refreshRefinementControls();
}

GenerationPreviewDialog::~GenerationPreviewDialog() {
    for (const auto &connection : m_refinementConnections) disconnect(connection);
    if (m_refinementService && !m_pendingRefinement.generation.jobId.isEmpty())
        m_refinementService->cancel(m_pendingRefinement.generation.jobId);
    if (m_audio && m_audio->pcmSnapshot().revision == m_draft.source.audioRevision) {
        m_audio->setLoop(m_initialLoopStart, m_initialLoopEnd, m_initialLoopEnabled);
        if (m_changedPlayback) {
            m_audio->pause();
            m_audio->seek(m_initialPosition);
            if (m_initialPlaying) m_audio->play();
        }
    }
}

void GenerationPreviewDialog::showApplicationError(const QString &message) {
    m_status->setText(message);
    m_status->setProperty("role", "error");
    m_status->show();
}

void GenerationPreviewDialog::displayDraft() {
    QVector<EditorObject> objects;
    int directional = 0, dots = 0, bombs = 0, walls = 0;
    for (const auto &source : m_draft.objects) {
        EditorObject object;
        object.id = source.id; object.beat = source.beat; object.type = int(source.kind);
        object.x = source.x; object.y = source.y; object.color = source.color; object.direction = source.direction;
        object.duration = source.duration; object.width = source.width; object.height = source.height;
        object.locked = source.isProtected(); object.protectedReason = source.protectedReason;
        objects.append(object);
        if (source.kind == ObjectKind::Bomb) ++bombs;
        else if (source.kind == ObjectKind::Wall) ++walls;
        else if (source.direction == 8) ++dots;
        else ++directional;
    }
    m_track->setObjects(objects); m_timeline->setObjects(objects);
    m_counts->setText(tr("%1 · 方向 %2 · 无方向 %3 · 炸弹 %4 · 墙 %5\n平均每秒 %6 个音符 · 峰值 %7")
        .arg(m_showRefinement ? tr("精修谱") : tr("初稿")).arg(directional).arg(dots).arg(bombs).arg(walls)
        .arg(m_draft.metrics.averageNps, 0, 'f', 2).arg(m_draft.metrics.peakNps, 0, 'f', 2));
    m_warnings->setText(m_draft.warnings.join('\n')); m_warnings->setVisible(!m_draft.warnings.isEmpty());
}

void GenerationPreviewDialog::setSourceValidation(std::function<bool(const GenerationRequest &)> validation) {
    m_sourceValidation = std::move(validation);
    refreshRefinementControls();
}

void GenerationPreviewDialog::setDocumentBaseline(bool documentBaseline) {
    m_documentBaseline = documentBaseline;
    auto notice = findChild<QLabel *>(QStringLiteral("generationReplaceNotice"));
    if (notice && documentBaseline) notice->setText(tr("应用精修时仅写入已审核的修改、新增和删除；其他物件、难度、音频及未知字段保持原样，全部变化可一次撤销。"));
    refreshRefinementControls();
}

void GenerationPreviewDialog::setRefinementSelection(double startSeconds, double endSeconds) {
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds)) return;
    const double duration = m_initialDraft.source.audio.durationSeconds;
    const double start = qBound(0.0, qMin(startSeconds, endSeconds), duration);
    const double end = qBound(0.0, qMax(startSeconds, endSeconds), duration);
    const QSignalBlocker a(m_rangeStart), b(m_rangeEnd), c(m_selectedOnly);
    m_rangeStart->setValue(start); m_rangeEnd->setValue(end); m_selectedOnly->setChecked(true);
    m_timeline->setLoop(start, end); refreshRefinementControls();
}

bool GenerationPreviewDialog::acceptsRefinement(const RefinementResult &result) const {
    const auto &source = result.source.generation;
    const auto &expected = m_pendingRefinement.generation;
    const auto validation = m_sourceValidation;
    return !expected.jobId.isEmpty() && sameGenerationSource(source, expected)
        && sameGenerationSource(result.candidate.source, source)
        && result.source.baselineHash == m_pendingRefinement.baselineHash
        && refinementBaselineHash(result.source.baseline) == m_pendingRefinement.baselineHash
        && result.source.selectedOnly == m_pendingRefinement.selectedOnly
        && result.source.startSeconds == m_pendingRefinement.startSeconds
        && result.source.endSeconds == m_pendingRefinement.endSeconds
        && result.source.maximumSegments == m_pendingRefinement.maximumSegments
        && (!validation || validation(source));
}

void GenerationPreviewDialog::setRefinementService(AiRefinementService *service) {
    if (service == m_refinementService) { refreshRefinementControls(); return; }
    QPointer<GenerationPreviewDialog> guard(this);
    cancelRefinement();
    if (!guard) return;
    for (const auto &connection : m_refinementConnections) disconnect(connection);
    m_refinementConnections.clear(); m_refinementService = service;
    const auto revision = ++m_refinementServiceRevision;
    if (service) {
        m_refinementConnections.append(connect(this, &GenerationPreviewDialog::refinementRequested, service, &AiRefinementService::refine));
        m_refinementConnections.append(connect(this, &GenerationPreviewDialog::cancelRefinementRequested, service, &AiRefinementService::cancel));
        m_refinementConnections.append(connect(service, &AiRefinementService::candidateReady, this,
            [this, revision](const RefinementResult &result) {
                QPointer<GenerationPreviewDialog> guard(this);
                if (revision != m_refinementServiceRevision || !acceptsRefinement(result) || !guard) return;
                m_pendingRefinement = {};
                setRefinementResult(result);
                if (guard) emit refinementCompleted(result);
            }));
        m_refinementConnections.append(connect(service, &AiRefinementService::progress, this,
            [this, revision](const QString &job, int percent, const QString &stage) {
                if (revision != m_refinementServiceRevision || job.isEmpty() || job != m_pendingRefinement.generation.jobId) return;
                m_refinementProgress->setRange(0, percent < 0 ? 0 : 100);
                if (percent >= 0) m_refinementProgress->setValue(qBound(0, percent, 100));
                m_status->setText(stage); m_status->setProperty("role", "status"); m_status->show();
            }));
        m_refinementConnections.append(connect(service, &AiRefinementService::requestFailed, this,
            [this, revision](const QString &job, const QString &message) {
                QPointer<GenerationPreviewDialog> guard(this);
                const bool currentJob = job == m_pendingRefinement.generation.jobId
                    || (m_pendingRefinement.generation.jobId.isEmpty() && job == m_lastRefinement.generation.jobId);
                if (revision != m_refinementServiceRevision || job.isEmpty() || !currentJob) return;
                const auto validation = m_sourceValidation;
                const auto source = m_lastRefinement.generation;
                const bool current = !validation || validation(source);
                if (!guard || !current) return;
                m_pendingRefinement = {};
                showApplicationError(message+tr("\n初稿与已完成的精修结果已保留。"));
                refreshRefinementControls();
            }));
        m_refinementConnections.append(connect(service, &AiRefinementService::cancelled, this,
            [this, revision](const QString &job) {
                if (revision != m_refinementServiceRevision || job.isEmpty() || job != m_pendingRefinement.generation.jobId) return;
                m_pendingRefinement = {}; m_status->setText(tr("精修已停止，初稿与已完成片段保留。")); m_status->show();
                refreshRefinementControls();
            }));
        m_refinementConnections.append(connect(service, &AiRefinementService::availabilityChanged, this, [this, revision] {
            if (revision != m_refinementServiceRevision) return;
            QPointer<GenerationPreviewDialog> guard(this);
            if (!m_refinementService || !m_refinementService->isAvailable()) cancelRefinement();
            if (guard) refreshRefinementControls();
        }));
        m_refinementConnections.append(connect(service, &QObject::destroyed, this, [this, revision] {
            if (revision != m_refinementServiceRevision) return;
            m_pendingRefinement = {}; m_refinementService.clear();
            showApplicationError(tr("AI 精修服务已断开，初稿仍可预览和应用。")); refreshRefinementControls();
        }));
    }
    refreshRefinementControls();
}

void GenerationPreviewDialog::setRefinementResult(const RefinementResult &result) {
    QPointer<GenerationPreviewDialog> guard(this);
    if (result.source.generation.jobId.isEmpty()
            || !sameGenerationSource(result.source.generation, m_initialDraft.source, false)
            || !sameGenerationSource(result.candidate.source, result.source.generation)) return;
    const auto validation = m_sourceValidation;
    const bool current = !validation || validation(result.source.generation);
    if (!guard || !current) return;
    if (refinementBaselineHash(result.source.baseline) != refinementBaselineHash(m_initialDraft.objects)
            || result.source.baselineHash != refinementBaselineHash(m_initialDraft.objects)) return;
    m_refinement = result; m_hasRefinement = true; m_showRefinement = true;
    m_lastRefinement = result.source;
    m_draft = result.candidate;
    const QSignalBlocker blocker(m_comparison); m_comparison->setCurrentIndex(1);
    m_refinementStats->setText(tr("修改 %1 · 移动拍点 %2 · 新增 %3 · 删除 %4\n已检查 %5 个乐句 · 剩余 %6 个乐句%7")
        .arg(result.stats.changed).arg(result.stats.moved).arg(result.stats.added).arg(result.stats.removed)
        .arg(result.processedSegments.size()).arg(result.remainingSegments.size())
        .arg(result.source.selectedOnly ? tr(" · 选段 %1–%2 秒").arg(result.source.startSeconds,0,'f',3).arg(result.source.endSeconds,0,'f',3) : tr(" · 全曲")));
    if (!result.processedRanges.isEmpty()) m_refinementStats->setText(m_refinementStats->text()+tr("\n实际已精修：%1").arg(result.processedRanges.join(QStringLiteral("；"))));
    m_status->setText(result.resumable ? tr("本轮精修完成，可比较初稿并继续剩余片段。") : tr("精修完成，请试听比较后应用。"));
    m_status->setProperty("role", "success"); m_status->show();
    m_refinementProgress->setRange(0,100); m_refinementProgress->setValue(100);
    displayDraft(); refreshRefinementControls();
}

void GenerationPreviewDialog::refreshRefinementControls() {
    QPointer<GenerationPreviewDialog> guard(this);
    const bool running = isRefining();
    const bool available = m_refinementService && m_refinementService->isAvailable();
    if (!guard) return;
    const auto validation = m_sourceValidation;
    const auto source = m_initialDraft.source;
    const bool current = !validation || validation(source);
    if (!guard) return;
    bool validBaseline = !m_initialDraft.objects.isEmpty();
    QSet<QString> ids;
    for (const auto &object : m_initialDraft.objects) {
        if (object.id.isEmpty() || ids.contains(object.id) || object.isProtected()) validBaseline = false;
        ids.insert(object.id);
    }
    const bool validRange = !m_selectedOnly->isChecked() || m_rangeEnd->value() > m_rangeStart->value();
    m_refinementScroll->setVisible(m_refinementService || m_hasRefinement || !m_lastRefinement.generation.jobId.isEmpty());
    m_refine->setEnabled(available && current && validBaseline && validRange && !running);
    m_refine->setToolTip(available ? QString() : tr("请先在大语言模型页面配置并保存 AI 连接。"));
    const auto state = m_refinementService ? m_refinementService->status() : AiGenerationService::Status{};
    if (!guard) return;
    const bool resumable = !m_lastRefinement.generation.jobId.isEmpty()
        && state.jobId == m_lastRefinement.generation.jobId && state.resumable;
    m_resumeRefinement->setVisible(resumable);
    m_resumeRefinement->setEnabled(resumable && available && current && !running);
    m_cancelRefinement->setVisible(running); m_cancelRefinement->setEnabled(running);
    m_comparison->setEnabled(m_hasRefinement); m_restore->setEnabled(m_hasRefinement || running);
    m_selectedOnly->setEnabled(!running);
    for (auto spin : {m_rangeStart,m_rangeEnd}) spin->setEnabled(m_selectedOnly->isChecked() && !running);
    m_refinementProgress->setVisible(running || m_hasRefinement);
    m_apply->setEnabled(current && !running && !m_draft.objects.isEmpty() && (!m_documentBaseline || hasRefinementPatch()));
}

void GenerationPreviewDialog::refineChart() {
    QPointer<GenerationPreviewDialog> guard(this);
    refreshRefinementControls(); if (!guard || !m_refine->isEnabled()) return;
    RefinementRequest request;
    request.generation = m_initialDraft.source;
    request.generation.jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.baseline = m_initialDraft.objects; request.baselineHash = refinementBaselineHash(request.baseline);
    request.selectedOnly = m_selectedOnly->isChecked();
    request.startSeconds = request.selectedOnly ? m_rangeStart->value() : 0;
    request.endSeconds = request.selectedOnly ? m_rangeEnd->value() : m_initialDraft.source.audio.durationSeconds;
    request.maximumSegments = 5;
    m_pendingRefinement = request; m_lastRefinement = request;
    m_refinementProgress->setRange(0,0); m_status->setText(tr("正在精修重点乐句，初稿已保留…")); m_status->show();
    refreshRefinementControls();
    if (guard) emit refinementRequested(request);
}

void GenerationPreviewDialog::resumeRefinement() {
    QPointer<GenerationPreviewDialog> guard(this);
    refreshRefinementControls(); if (!guard || !m_resumeRefinement->isEnabled() || !m_refinementService) return;
    m_pendingRefinement = m_lastRefinement; const QString job = m_lastRefinement.generation.jobId;
    m_status->setText(tr("正在继续精修剩余乐句…")); m_status->show(); refreshRefinementControls();
    if (guard && m_refinementService) m_refinementService->resume(job);
}

void GenerationPreviewDialog::cancelRefinement() {
    if (!isRefining()) return;
    const QString job = m_pendingRefinement.generation.jobId;
    QPointer<GenerationPreviewDialog> guard(this);
    emit cancelRefinementRequested(job);
    if (!guard) return;
    m_pendingRefinement = {};
    m_status->setText(tr("精修已停止，初稿与已完成结果保留。")); m_status->show();
    refreshRefinementControls();
}

void GenerationPreviewDialog::closeEvent(QCloseEvent *event) {
    QPointer<GenerationPreviewDialog> guard(this);
    cancelRefinement();
    if (guard) QDialog::closeEvent(event);
}

void GenerationPreviewDialog::restoreInitialDraft() {
    QPointer<GenerationPreviewDialog> guard(this); cancelRefinement(); if (!guard) return;
    const QString job = m_lastRefinement.generation.jobId;
    auto service = m_refinementService;
    m_lastRefinement = {}; m_refinement = {}; m_hasRefinement = false; m_showRefinement = false;
    m_draft = m_initialDraft; m_refinementStats->clear();
    { const QSignalBlocker blocker(m_comparison); m_comparison->setCurrentIndex(0); }
    displayDraft(); refreshRefinementControls();
    if (!guard) return;
    m_status->setText(tr("已恢复初稿，工程尚未改变。")); m_status->show();
    if (service && !job.isEmpty()) service->discard(job);
    if (guard) emit initialDraftRestored();
}

void GenerationPreviewDialog::invalidateRefinementForConnectionChange() {
    restoreInitialDraft();
}
}
