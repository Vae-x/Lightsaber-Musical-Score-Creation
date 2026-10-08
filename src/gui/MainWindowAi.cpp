#include "MainWindow.h"
#include "AiRecognitionPage.h"
#include "EditorRefinementPanel.h"
#include "EditorSessionController.h"
#include "core/AiRefinementService.h"
#include "core/BeatmapPlayabilityValidator.h"
#include "core/AudioService.h"
#include "core/GenerationSource.h"
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QUuid>

namespace {
bool sameGenerationTiming(const lmsc::TimeMap &a, const lmsc::TimeMap &b) {
    if (a.baseBpm() != b.baseBpm() || a.firstBeatSeconds() != b.firstBeatSeconds()
        || a.changes().size() != b.changes().size()) return false;
    for (int i = 0; i < a.changes().size(); ++i)
        if (a.changes()[i].beat != b.changes()[i].beat || a.changes()[i].bpm != b.changes()[i].bpm) return false;
    return true;
}
}

lmsc::BeatmapDocument *MainWindow::editingDocument() const {
    return m_editorSession ? m_editorSession->activeDocument() : m_document.get();
}

bool MainWindow::prepareManualEdit() {
    if (m_busy || !editingDocument()->isLoaded() || m_editorSession->viewReadOnly()) return false;
    // cancel() publishes already accepted segments synchronously. Merge those
    // before invalidating the request, then perform the user's edit.
    cancelRefinement(true);
    return !m_editorSession->viewReadOnly();
}

bool MainWindow::syncDraftRecords() {
    QString error;
    if (!m_editorSession->synchronizeRecords(&error)) {
        statusBar()->showMessage(error, 15000);
        return false;
    }
    return true;
}

void MainWindow::editingChanged() {
    if (m_editorSession->view() == lmsc::EditorSessionController::View::Working)
        m_editorSession->notifyWorkingChanged();
    else if (m_editorSession->view() == lmsc::EditorSessionController::View::Formal) {
        for (const auto &difficulty : m_document->difficulties())
            if (difficulty.id == m_document->currentDifficultyId()) {
                m_editorSession->selectTarget(difficulty.name);
                m_editorSession->setView(lmsc::EditorSessionController::View::Formal);
                break;
            }
    }
    syncDraftRecords();
    refreshDocument();
}

void MainWindow::showGenerationTools() {
    m_workspacePages->setCurrentWidget(m_editorPage);
    m_toolsTabs->setCurrentWidget(m_generationTools);
    if (m_mobileEditorTabs) m_mobileEditorTabs->setCurrentIndex(4);
    m_aiPage->setExpanded(true);
    refreshRecognitionContext();
    refreshDraftPanel();
}

void MainWindow::confirmDraftReplacement(const QString &message, std::function<void()> action) {
    if (m_busy) return;
    if (!m_editorSession->hasDraft()) { action(); return; }
    if (m_workspacePages->currentWidget() == m_editorPage) showGenerationTools();
    else m_toolsTabs->setCurrentWidget(m_generationTools);
    m_draftReplacementLabel->setText(message);
    const auto documentId = m_documentId;
    const auto target = m_editorSession->currentTargetKey();
    const auto revision = m_document->revision();
    const auto workingRevision = m_editorSession->workingDocument() ? m_editorSession->workingDocument()->revision() : 0;
    m_pendingDraftAction = [this, action = std::move(action), documentId, target, revision, workingRevision] {
        if (documentId != m_documentId || target != m_editorSession->currentTargetKey()
            || revision != m_document->revision()
            || workingRevision != (m_editorSession->workingDocument() ? m_editorSession->workingDocument()->revision() : 0)) {
            m_refinementPanel->showError(tr("提示对应的曲谱或草稿已改变，未执行替换。")); return;
        }
        action();
    };
    m_draftReplacement->show();
}

bool MainWindow::generationSourceIsCurrent(const lmsc::GenerationRequest &source) const {
    if (m_busy || !m_document->isLoaded() || !isAudioReady()) return false;
    const auto audio = m_audio->pcmSnapshot();
    return !source.jobId.isEmpty() && source.documentId == m_documentId
        && source.documentRevision == m_document->revision()
        && source.difficultyId == m_document->currentDifficultyId()
        && source.audioRevision == audio.revision && source.audio.revision == audio.revision
        && source.audio.path == audio.path && source.audio.sourcePath == audio.sourcePath
        && source.audio.durationSeconds == audio.durationSeconds
        && source.audio.sampleRate == audio.sampleRate && source.audio.channels == audio.channels
        && sameGenerationTiming(source.timeMap, m_document->timeMap());
}

void MainWindow::previewGeneratedChart(const lmsc::GenerationDraft &draft) {
    if (draft.source.analysisOnly || !m_document->isNewSong() || !generationSourceIsCurrent(draft.source)) return;
    auto adopt = [this, draft] {
        if (!generationSourceIsCurrent(draft.source)) {
            m_refinementPanel->showError(tr("歌曲或时间参数已经改变，请重新生成。")); return;
        }
        cancelRefinement(true);
        QString error;
        if (!m_editorSession->acceptGenerationDraft(draft, &error)) {
            m_refinementPanel->showError(error); return;
        }
        m_selection.clear(); m_refinementPanel->clearResult(); syncDraftRecords();
        if (m_workspacePages->currentWidget() == m_editorPage) showGenerationTools();
        else m_toolsTabs->setCurrentWidget(m_generationTools);
        m_aiPage->setExpanded(false);
        refreshDocument();
        statusBar()->showMessage(tr("工作草稿已就绪，可直接手动编辑、试听和精修；确认应用后才会导出。"), 15000);
    };
    // A returned candidate may target another difficulty. Keep all existing
    // sessions, and only request replacement for this target's manual work.
    // A cancelled service can synchronously publish passed segments. Receive
    // them while their original target is still selected.
    cancelRefinement(true);
    m_editorSession->selectTarget(draft.source.profile.name);
    if (m_editorSession->hasDraft()
        && m_editorSession->generationDraft().source.jobId == draft.source.jobId) {
        m_editorSession->setView(lmsc::EditorSessionController::View::Working);
        m_refinementPanel->clearResult();
        if (m_workspacePages->currentWidget() == m_editorPage) showGenerationTools();
        else m_toolsTabs->setCurrentWidget(m_generationTools);
        m_aiPage->setExpanded(false);
        refreshDocument();
        return;
    }
    const bool replace = m_editorSession->hasDraft() && !m_editorSession->isApplied();
    if (replace) confirmDraftReplacement(tr("新的生成结果将替换 %1 的现有工作草稿。")
                                         .arg(draft.source.profile.name), adopt);
    else adopt();
}

bool MainWindow::currentChartSupportsRefinement() const {
    const auto document = editingDocument();
    if (m_busy || !m_document->isNewSong() || !document->isLoaded() || !isAudioReady()
        || !document->readOnlyReason().isEmpty() || !document->timeMap().changes().isEmpty()
        || document->objects().isEmpty() || m_editorSession->viewReadOnly()) return false;
    for (const auto &object : document->objects()) if (object.isProtected()) return false;
    for (const auto &difficulty : document->difficulties())
        if (difficulty.id == document->currentDifficultyId())
            return difficulty.characteristic == QStringLiteral("Standard") && difficulty.version == QStringLiteral("2.2.0");
    return false;
}

void MainWindow::refineCurrentChart() {
    showGenerationTools();
    startRefinement(false, 0, m_audio->duration());
}

void MainWindow::startRefinement(bool selectedOnly, double start, double end) {
    if (!m_pendingRefinement.generation.jobId.isEmpty() || !currentChartSupportsRefinement()
        || !m_refinementService || !m_refinementService->isAvailable()) return;
    if (m_editorSession->view() == lmsc::EditorSessionController::View::Formal
        && m_editorSession->hasDraft() && !m_editorSession->isApplied()) {
        confirmDraftReplacement(tr("从正式谱重新精修将替换该难度现有的工作草稿。"), [this, selectedOnly, start, end] {
            m_editorSession->discardDraft(); startRefinement(selectedOnly, start, end);
        }); return;
    }
    auto draft = m_editorSession->generationDraft();
    auto source = draft.source;
    source.jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    source.documentId = m_documentId; source.documentRevision = m_document->revision();
    source.difficultyId = m_document->currentDifficultyId(); source.timeMap = editingDocument()->timeMap();
    source.audio = m_audio->pcmSnapshot(); source.audioRevision = source.audio.revision;
    if (m_editorSession->view() == lmsc::EditorSessionController::View::Formal) {
        for (const auto &difficulty : m_document->difficulties())
            if (difficulty.id == source.difficultyId) source.profile = lmsc::DifficultyProfile::forName(difficulty.name);
        QString error;
        if (!m_editorSession->beginRefinementDraft(source, &error)) { m_refinementPanel->showError(error); return; }
    }
    if (!m_editorSession->isApplicable()) { refreshDraftPanel(); return; }
    lmsc::RefinementRequest request;
    request.generation = source;
    request.baseline = m_editorSession->workingDocument()->objects();
    request.baselineHash = lmsc::refinementBaselineHash(request.baseline);
    request.generation.allowedTypes = {};
    for (const auto &object : request.baseline) {
        if (object.kind == lmsc::ObjectKind::Bomb) request.generation.allowedTypes |= lmsc::BombType;
        else if (object.kind == lmsc::ObjectKind::Wall) request.generation.allowedTypes |= lmsc::WallType;
        else request.generation.allowedTypes |= object.direction == 8 ? lmsc::DotType : lmsc::DirectionalType;
    }
    request.selectedOnly = selectedOnly;
    request.startSeconds = selectedOnly ? qBound(0.0, start, source.audio.durationSeconds) : 0;
    request.endSeconds = selectedOnly ? qBound(0.0, end, source.audio.durationSeconds) : source.audio.durationSeconds;
    request.maximumSegments = 5;
    if (request.endSeconds <= request.startSeconds) { m_refinementPanel->showError(tr("精修终点必须晚于起点。")); return; }
    QString error;
    if (!m_editorSession->beginRefinement(false, &error)) { m_refinementPanel->showError(error); return; }
    m_cachedRefinement.reset(); m_refinementPanel->clearResult();
    m_aiPage->setExpanded(false);
    m_pendingRefinement = m_lastRefinement = request;
    m_refinementTarget = m_editorSession->currentTargetKey();
    m_refinementWorkingRevision = m_editorSession->workingDocument()->revision();
    m_refinementPanel->setProgress(-1, tr("正在精修重点乐句，初稿已保留…"));
    syncDraftRecords(); refreshDocument();
    m_refinementService->refine(request);
}

bool MainWindow::acceptsRefinement(const lmsc::RefinementResult &result) const {
    return !m_pendingRefinement.generation.jobId.isEmpty()
        && m_editorSession->hasDraft() && m_editorSession->isApplicable()
        && m_editorSession->currentTargetKey() == m_refinementTarget
        && m_editorSession->workingDocument()->revision() == m_refinementWorkingRevision
        && generationSourceIsCurrent(result.source.generation)
        && lmsc::sameGenerationSource(result.source.generation, m_pendingRefinement.generation)
        && lmsc::sameGenerationSource(result.candidate.source, result.source.generation)
        && result.source.baselineHash == m_pendingRefinement.baselineHash
        && lmsc::refinementBaselineHash(result.source.baseline) == m_pendingRefinement.baselineHash
        && result.source.selectedOnly == m_pendingRefinement.selectedOnly
        && result.source.startSeconds == m_pendingRefinement.startSeconds
        && result.source.endSeconds == m_pendingRefinement.endSeconds
        && result.source.maximumSegments == 5;
}

void MainWindow::cancelRefinement(bool manualEdit) {
    const auto job = m_pendingRefinement.generation.jobId;
    const auto lastJob = m_lastRefinement.generation.jobId;
    if (m_refinementService && !job.isEmpty()) m_refinementService->cancel(job);
    m_pendingRefinement = {};
    if (manualEdit) {
        m_lastRefinement = {}; m_cachedRefinement.reset();
        if (m_refinementService && !lastJob.isEmpty()) m_refinementService->discard(lastJob);
    }
    if (m_refinementPanel) refreshDraftPanel();
}

void MainWindow::resumeRefinement() {
    if (m_busy || !m_cachedRefinement || !m_cachedRefinement->resumable || !m_refinementService
        || !m_pendingRefinement.generation.jobId.isEmpty() || !m_editorSession->isApplicable()
        || m_refinementTarget != m_editorSession->currentTargetKey()
        || !generationSourceIsCurrent(m_lastRefinement.generation)
        || lmsc::refinementBaselineHash(m_editorSession->workingDocument()->objects())
            != lmsc::refinementBaselineHash(m_cachedRefinement->candidate.objects)) return;
    QString error;
    if (!m_editorSession->beginRefinement(true, &error)) { m_refinementPanel->showError(error); return; }
    m_pendingRefinement = m_lastRefinement;
    m_refinementWorkingRevision = m_editorSession->workingDocument()->revision();
    m_refinementPanel->setProgress(-1, tr("正在继续精修剩余乐句…"));
    refreshDraftPanel(); m_refinementService->resume(m_lastRefinement.generation.jobId);
}

void MainWindow::setAiRefinementService(lmsc::AiRefinementService *service) {
    cancelRefinement(true);
    for (const auto &connection : m_refinementConnections) disconnect(connection);
    m_refinementConnections.clear();
    m_refinementService = service ? service : m_defaultRefinementService;
    connectRefinementService(); refreshDraftPanel();
}

void MainWindow::connectRefinementService() {
    const auto revision = ++m_refinementServiceRevision;
    auto service = m_refinementService.data();
    if (!service) return;
    m_refinementConnections.append(connect(service, &lmsc::AiRefinementService::candidateReady, this,
        [this, revision](const lmsc::RefinementResult &result) {
            if (revision != m_refinementServiceRevision || !acceptsRefinement(result)) return;
            QString error;
            if (!m_editorSession->mergeRefinementResult(result, &error)) {
                m_pendingRefinement = {}; m_refinementPanel->showError(error); refreshDraftPanel(); return;
            }
            m_pendingRefinement = {}; m_lastRefinement = result.source;
            m_cachedRefinement.reset(new lmsc::RefinementResult(result));
            m_refinementWorkingRevision = m_editorSession->workingDocument()->revision();
            m_refinementPanel->setResult(result); m_refinementPanel->setProgress(100, tr("本轮精修完成"));
            syncDraftRecords(); refreshDocument();
        }));
    m_refinementConnections.append(connect(service, &lmsc::AiRefinementService::progress, this,
        [this, revision](const QString &job, int percent, const QString &stage) {
            if (revision == m_refinementServiceRevision && !job.isEmpty() && job == m_pendingRefinement.generation.jobId)
                m_refinementPanel->setProgress(percent, stage);
        }));
    m_refinementConnections.append(connect(service, &lmsc::AiRefinementService::requestFailed, this,
        [this, revision](const QString &job, const QString &message) {
            if (revision != m_refinementServiceRevision || job.isEmpty()
                || (job != m_pendingRefinement.generation.jobId && job != m_lastRefinement.generation.jobId)) return;
            m_pendingRefinement = {}; refreshDraftPanel();
            m_refinementPanel->showError(message + tr("\n初稿与已通过的修改已保留。"));
        }));
    m_refinementConnections.append(connect(service, &lmsc::AiRefinementService::cancelled, this,
        [this, revision](const QString &job) {
            if (revision != m_refinementServiceRevision || job.isEmpty() || job != m_pendingRefinement.generation.jobId) return;
            m_pendingRefinement = {}; refreshDraftPanel();
            m_refinementPanel->setProgress(0, tr("精修已停止，已完成修改保留。"));
        }));
    m_refinementConnections.append(connect(service, &lmsc::AiRefinementService::availabilityChanged, this, [this, revision] {
        if (revision != m_refinementServiceRevision) return;
        if (!m_refinementService || !m_refinementService->isAvailable()) cancelRefinement(true);
        refreshDraftPanel();
    }));
    m_refinementConnections.append(connect(service, &QObject::destroyed, this, [this, revision] {
        if (revision != m_refinementServiceRevision) return;
        m_pendingRefinement = {}; m_lastRefinement = {}; m_cachedRefinement.reset();
        m_refinementService.clear(); refreshDraftPanel();
    }));
}

void MainWindow::refreshDraftPanel() {
    if (!m_refinementPanel) return;
    using Controller = lmsc::EditorSessionController;
    const auto view = m_editorSession->view();
    const auto target = m_editorSession->currentTargetKey();
    QString viewName;
    switch (view) {
    case Controller::View::Formal: viewName = tr("正式谱"); break;
    case Controller::View::Working: viewName = m_editorSession->isApplied() ? tr("已应用候选 · 只读") : tr("工作草稿"); break;
    case Controller::View::Initial: viewName = tr("初稿 · 只读"); break;
    case Controller::View::BeforeRefinement: viewName = tr("精修前 · 只读"); break;
    }
    m_editorStateLabel->setText(target.isEmpty() ? viewName : tr("%1 · %2").arg(target, viewName));
    lmsc::EditorRefinementPanel::State state;
    state.available = m_refinementService && m_refinementService->isAvailable();
    state.sourceValid = !m_busy && m_document->isNewSong() && isAudioReady()
        && (!m_editorSession->hasDraft() || m_editorSession->isApplicable() || m_editorSession->isApplied());
    state.hasCandidate = m_editorSession->hasDraft();
    state.hasRefinement = m_editorSession->hasBeforeRefinement();
    state.running = !m_pendingRefinement.generation.jobId.isEmpty();
    state.canRefine = state.available && currentChartSupportsRefinement();
    QString applicationReason;
    state.canApply = !m_busy && view == Controller::View::Working
        && m_editorSession->canApplyDraft(&applicationReason);
    state.manualModified = m_editorSession->isManualModified();
    state.durationSeconds = m_audio->duration();
    state.version = static_cast<lmsc::EditorRefinementPanel::Version>(view);
    const auto status = m_refinementService ? m_refinementService->status() : lmsc::AiGenerationService::Status{};
    state.resumable = m_cachedRefinement && m_cachedRefinement->resumable
        && status.resumable && status.jobId == m_lastRefinement.generation.jobId
        && m_refinementTarget == target && !m_editorSession->viewReadOnly()
        && generationSourceIsCurrent(m_lastRefinement.generation);
    if (!state.available) state.unavailableReason = tr("请先在大语言模型设置中配置并保存 AI 连接。");
    state.summary = m_editorSession->warnings().join('\n');
    if (state.hasCandidate) {
        const auto draft = m_editorSession->generationDraft();
        if (!draft.summary.isEmpty()) state.summary = draft.summary + QStringLiteral("\n") + state.summary;
        if (!draft.warnings.isEmpty()) state.summary += QStringLiteral("\n") + draft.warnings.join('\n');
        if (!applicationReason.isEmpty() && view == Controller::View::Working && !m_editorSession->isApplied())
            state.summary += QStringLiteral("\n") + applicationReason;
        state.summary += (state.summary.isEmpty() ? QString() : QStringLiteral("\n"))
            + tr("未应用草稿不会导出。保存工程会同时保存工作草稿和对比版本。");
    }
    m_refinementPanel->setState(state);
}
