#include "core/AiGenerationService.h"
#include "core/AiRefinementService.h"
#include "gui/AiRecognitionPage.h"
#include "gui/EditorViews.h"
#include "gui/GenerationPreviewDialog.h"
#include "gui/ThemeManager.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFont>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QStandardItemModel>

namespace {
class FakeGenerationService final : public lmsc::AiGenerationService {
public:
    bool available = true;
    QVector<lmsc::GenerationRequest> requests;
    QStringList cancelledJobs;
    Status current;
    QStringList resumedJobs;
    Status status() const override { return current; }
    bool isAvailable() const override { return available; }
    void generate(const lmsc::GenerationRequest &request) override { requests.append(request); current={}; current.jobId=request.jobId; current.state=Status::Running; }
    void cancel(const QString &jobId) override { cancelledJobs.append(jobId); current.state=Status::Paused; current.resumable=true; emit cancelled(jobId); }
    void discard(const QString &jobId) override { cancelledJobs.append(jobId); current={}; }
    void resume(const QString &jobId) override { resumedJobs.append(jobId); current.state=Status::Running; current.resumable=false; emit progress(jobId,50,QStringLiteral("继续乐句")); }
    void finish(const lmsc::GenerationRequest &source, const QString &summary = QStringLiteral("测试规划"), bool themeWarning = false) {
        current.state=Status::Completed; current.resumable=false;
        lmsc::GenerationDraft draft;
        draft.source = source;
        draft.summary = summary;
        draft.hasThemeWarnings=themeWarning;
        if (themeWarning) draft.warnings.append(QStringLiteral("乐句 7（48.0–56.0 秒）动作主题差异 0.25，目标 0.10；建议试听。"));
        if (!source.analysisOnly) {
            lmsc::BeatObject note;
            note.beat = 2.0; note.x = 0; note.y = 1; note.direction = 1;
            draft.objects.append(note);
            draft.metrics.directional = 1;
        }
        emit draftReady(draft);
    }
};
class FakeRefinementService final : public lmsc::AiRefinementService {
public:
    FakeRefinementService() : AiRefinementService(nullptr) {}
    bool available = true;
    QVector<lmsc::RefinementRequest> requests;
    QStringList cancelledJobs, discardedJobs, resumedJobs;
    lmsc::AiGenerationService::Status current;
    bool isAvailable() const override { return available; }
    lmsc::AiGenerationService::Status status() const override { return current; }
    void refine(const lmsc::RefinementRequest &request) override { requests.append(request); current={}; current.jobId=request.generation.jobId; current.state=lmsc::AiGenerationService::Status::Running; }
    void cancel(const QString &job) override { cancelledJobs.append(job); current.state=lmsc::AiGenerationService::Status::Paused; current.resumable=true; emit cancelled(job); }
    void discard(const QString &job) override { discardedJobs.append(job); current={}; }
    void resume(const QString &job) override { resumedJobs.append(job); current.state=lmsc::AiGenerationService::Status::Running; current.resumable=false; }
    lmsc::RefinementResult result(const lmsc::RefinementRequest &request, bool more = true) {
        lmsc::RefinementResult result; result.source=request; result.candidate.source=request.generation;
        result.candidate.objects=request.baseline;
        if (!result.candidate.objects.isEmpty()) result.candidate.objects.first().y=(result.candidate.objects.first().y+1)%3;
        result.patch=lmsc::refinementDifference(request.baseline,result.candidate.objects);
        result.stats=lmsc::refinementStatistics(result.patch,request.baseline);
        result.processedSegments=QStringList{QStringLiteral("s1")};
        result.processedRanges=QStringList{QStringLiteral("s1 · 2.000–8.000 秒")};
        if (more) result.remainingSegments=QStringList{QStringLiteral("s2")}; result.resumable=more;
        return result;
    }
    void finish(const lmsc::RefinementRequest &request, bool more = true) {
        current.jobId=request.generation.jobId; current.state=lmsc::AiGenerationService::Status::Completed; current.resumable=more;
        emit candidateReady(result(request,more));
    }
};
}

class AiGenerationPageTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void fiveDifficultiesAndTypeSelection();
    void analysisAndGenerationAreSeparate();
    void importedSongOnlyAllowsAnalysis();
    void changedOptionsAndDocumentDiscardOldDrafts();
    void alteredResultSourceIsRejected();
    void backendReplacementAndDestructionAreSafe();
    void settingsAndAvailabilityInvalidateRequests();
    void previewHasReadOnlyTimelineAndExplicitApplication();
    void busyAndNavigationPreserveProgressAndReusableDraft();
    void pausedJobCanResumeWithoutStartingOver();
    void themeWarningsSurviveNavigationAndPreview();
    void automaticRecoveryStaysBusyAcrossNavigation();
    void localModeIsDefaultAndDoesNotRequireModelConnection();
    void switchingModesCancelsAndRejectsOldResults();
    void modelChangesPreserveLocalTasksAndDrafts();
    void inactiveBackendDestructionAndSharedBackendsAreSafe();
    void hybridModeDefaultsToPlanningAndChangesSeed();
    void infernoModeRestrictsTypesAndRestoresOtherOptions();
    void infernoTasksIgnoreAiConnectionsAndRejectCancelledResults();
    void refinementPreviewComparesRestoresAndPreservesIds();
    void refinementCancelSettingsAndResumeRejectLateResults();
    void refinementCallbacksMayDestroyPreview();
    void refinementRejectsAlteredSource_data();
    void refinementRejectsAlteredSource();
private:
    lmsc::GenerationRequest context() const;
    void setup(lmsc::AiRecognitionPage &page, FakeGenerationService &service, bool newSong = true);
    QTemporaryDir m_temp;
    QString m_audio, m_pcm;
};

void AiGenerationPageTest::initTestCase() {
    QApplication::setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
    QVERIFY(m_temp.isValid());
    qRegisterMetaType<QSet<QString>>();
    m_audio = m_temp.filePath(QStringLiteral("fixture.ogg"));
    m_pcm = m_temp.filePath(QStringLiteral("fixture.pcm"));
    for (const auto &path : {m_audio, m_pcm}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("mock-only-local-fixture"), qint64(23));
    }
}

lmsc::GenerationRequest AiGenerationPageTest::context() const {
    lmsc::GenerationRequest request;
    request.documentId = QStringLiteral("test-document-instance");
    request.difficultyId = QStringLiteral("Standard:Expert.dat");
    request.documentRevision = 4;
    request.audioRevision = 7;
    request.audio.path = m_pcm;
    request.audio.sourcePath = m_audio;
    request.audio.revision = 7;
    request.audio.durationSeconds = 16;
    request.audio.lease = std::make_shared<int>(1);
    request.timeMap.configure(120, 0.25);
    request.profile = lmsc::DifficultyProfile::forName(QStringLiteral("Expert"));
    return request;
}

void AiGenerationPageTest::setup(lmsc::AiRecognitionPage &page, FakeGenerationService &service, bool newSong) {
    page.setGenerationService(&service);
    page.setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    page.setContext(m_audio, QStringLiteral("生成测试歌曲"), 120, 0.25, 16, false);
    page.setGenerationContext(context(), newSong, false);
}

void AiGenerationPageTest::fiveDifficultiesAndTypeSelection() {
    FakeGenerationService service;
    lmsc::AiRecognitionPage page;
    setup(page, service);
    auto difficulty = page.findChild<QComboBox *>(QStringLiteral("aiGenerationDifficulty"));
    auto directional = page.findChild<QCheckBox *>(QStringLiteral("aiDirectionalType"));
    auto dots = page.findChild<QCheckBox *>(QStringLiteral("aiDotType"));
    auto bombs = page.findChild<QCheckBox *>(QStringLiteral("aiBombType"));
    auto walls = page.findChild<QCheckBox *>(QStringLiteral("aiWallType"));
    auto generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto analyze = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    QVERIFY(difficulty && directional && dots && bombs && walls && generate && analyze);
    QCOMPARE(difficulty->count(), 5);
    QCOMPARE(difficulty->currentData().toString(), QStringLiteral("Expert"));
    QVERIFY(directional->isChecked());
    QVERIFY(!dots->isChecked() && !bombs->isChecked() && !walls->isChecked());
    QVERIFY(generate->isEnabled());
    directional->setChecked(false);
    QVERIFY(!generate->isEnabled());
    QVERIFY(analyze->isEnabled());
    bombs->setChecked(true);
    walls->setChecked(true);
    QVERIFY(generate->isEnabled());
    difficulty->setCurrentIndex(difficulty->findData(QStringLiteral("Hard")));
    generate->click();
    QCOMPARE(service.requests.size(), 1);
    QCOMPARE(service.requests.last().profile.name, QStringLiteral("Hard"));
    QCOMPARE(service.requests.last().profile.rank, 5);
    QCOMPARE(service.requests.last().allowedTypes, lmsc::GeneratedTypes(lmsc::BombType | lmsc::WallType));
    QVERIFY(!service.requests.last().analysisOnly);
    QCOMPARE(context().profile.name, QStringLiteral("Expert"));
}

void AiGenerationPageTest::analysisAndGenerationAreSeparate() {
    FakeGenerationService service;
    lmsc::AiRecognitionPage page;
    setup(page, service);
    auto generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto analyze = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto progress = page.findChild<QProgressBar *>(QStringLiteral("aiRecognitionProgress"));
    auto result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    QVERIFY(generate && analyze && progress && result);
    QSignalSpy drafts(&page, &lmsc::AiRecognitionPage::generationDraftReady);
    generate->click();
    QCOMPARE(service.requests.size(), 1);
    const auto generated = service.requests.last();
    QVERIFY(!generated.jobId.isEmpty());
    QCOMPARE(generated.documentRevision, quint64(4));
    QCOMPARE(generated.audioRevision, quint64(7));
    QCOMPARE(generated.audio.path, m_pcm);
    QVERIFY(generated.audio.lease);
    QVERIFY(page.isRecognizing());
    emit service.progress(generated.jobId, 42, QStringLiteral("正在规划动作"));
    QCOMPARE(progress->value(), 42);
    emit service.progress(QStringLiteral("old-job"), 90, QStringLiteral("过期进度"));
    QCOMPARE(progress->value(), 42);
    service.finish(generated);
    QCOMPARE(drafts.count(), 1);
    QVERIFY(!page.isRecognizing());
    QVERIFY(result->toPlainText().contains(QStringLiteral("测试规划")));
    analyze->click();
    QCOMPARE(service.requests.size(), 2);
    const auto analysis = service.requests.last();
    QVERIFY(analysis.analysisOnly);
    QVERIFY(analysis.jobId != generated.jobId);
    service.finish(analysis, QStringLiteral("仅分析音乐"));
    QCOMPARE(drafts.count(), 1);
    QVERIFY(result->toPlainText().contains(QStringLiteral("仅分析音乐")));
}

void AiGenerationPageTest::importedSongOnlyAllowsAnalysis() {
    FakeGenerationService service;
    lmsc::AiRecognitionPage page;
    setup(page, service, false);
    auto generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto analyze = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    QVERIFY(generate && analyze);
    QVERIFY(!generate->isEnabled());
    QVERIFY(analyze->isEnabled());
    generate->click();
    QCOMPARE(service.requests.size(), 0);
    analyze->click();
    QCOMPARE(service.requests.size(), 1);
    QVERIFY(service.requests.last().analysisOnly);
}

void AiGenerationPageTest::changedOptionsAndDocumentDiscardOldDrafts() {
    FakeGenerationService service;
    lmsc::AiRecognitionPage page;
    setup(page, service);
    auto generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto dots = page.findChild<QCheckBox *>(QStringLiteral("aiDotType"));
    auto result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    QSignalSpy drafts(&page, &lmsc::AiRecognitionPage::generationDraftReady);
    generate->click();
    const auto first = service.requests.last();
    dots->setChecked(true);
    QCOMPARE(service.cancelledJobs.last(), first.jobId);
    QVERIFY(!page.isRecognizing());
    service.finish(first);
    QCOMPARE(drafts.count(), 0);
    QVERIFY(result->toPlainText().isEmpty());
    generate->click();
    const auto second = service.requests.last();
    auto edited = context();
    ++edited.documentRevision;
    page.setGenerationContext(edited, true, false);
    QCOMPARE(service.cancelledJobs.last(), second.jobId);
    service.finish(second);
    QCOMPARE(drafts.count(), 0);
    generate->click();
    const auto third = service.requests.last();
    auto changedSong = edited;
    changedSong.documentId = QStringLiteral("another-document");
    changedSong.audioRevision = ++changedSong.audio.revision;
    page.setGenerationContext(changedSong, true, false);
    QCOMPARE(service.cancelledJobs.last(), third.jobId);
    service.finish(third);
    QCOMPARE(drafts.count(), 0);
}

void AiGenerationPageTest::alteredResultSourceIsRejected() {
    FakeGenerationService service;
    lmsc::AiRecognitionPage page;
    setup(page, service);
    auto generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    QSignalSpy drafts(&page, &lmsc::AiRecognitionPage::generationDraftReady);
    generate->click();
    const auto source = service.requests.last();
    auto altered = source;
    ++altered.audioRevision;
    service.finish(altered);
    QCOMPARE(drafts.count(), 0);
    QVERIFY(page.isRecognizing());
    altered = source;
    altered.profile.maxPeakNps += 2.0;
    service.finish(altered);
    QCOMPARE(drafts.count(), 0);
    altered = source;
    altered.allowedTypes = lmsc::WallType;
    service.finish(altered);
    QCOMPARE(drafts.count(), 0);
    service.finish(source);
    QCOMPARE(drafts.count(), 1);
}

void AiGenerationPageTest::backendReplacementAndDestructionAreSafe() {
    FakeGenerationService fallback;
    auto injected = new FakeGenerationService;
    lmsc::AiRecognitionPage page;
    page.setContext(m_audio, {}, 120, 0.25, 16, false);
    page.setGenerationContext(context(), true, false);
    page.setGenerationService(injected, &fallback);
    page.setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    auto generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    QVERIFY(!injected->parent());
    generate->click();
    const auto request = injected->requests.last();
    page.setGenerationService(&fallback, &fallback);
    QCOMPARE(injected->cancelledJobs.last(), request.jobId);
    injected->finish(request);
    QVERIFY(!page.isRecognizing());
    page.setGenerationService(injected, &fallback);
    generate->click();
    delete injected;
    QVERIFY(!page.isRecognizing());
    QVERIFY(generate->isEnabled());
    generate->click();
    QCOMPARE(fallback.requests.size(), 1);
}

void AiGenerationPageTest::settingsAndAvailabilityInvalidateRequests() {
    FakeGenerationService service;
    lmsc::AiRecognitionPage page;
    setup(page, service);
    auto generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    QSignalSpy drafts(&page, &lmsc::AiRecognitionPage::generationDraftReady);
    generate->click();
    const auto first = service.requests.last();
    page.invalidateGeneration();
    QCOMPARE(service.cancelledJobs.last(), first.jobId);
    service.finish(first);
    QCOMPARE(drafts.count(), 0);
    generate->click();
    const auto second = service.requests.last();
    service.available = false;
    emit service.availabilityChanged();
    QCOMPARE(service.cancelledJobs.last(), second.jobId);
    QVERIFY(!generate->isEnabled());
    service.finish(second);
    QCOMPARE(drafts.count(), 0);
    service.available = true;
    emit service.availabilityChanged();
    QVERIFY(generate->isEnabled());
}

void AiGenerationPageTest::previewHasReadOnlyTimelineAndExplicitApplication() {
    lmsc::GenerationDraft draft;
    draft.source = context();
    lmsc::BeatObject note;
    note.beat = 2; note.x = 0; note.y = 1; note.direction = 1;
    draft.objects.append(note);
    QPointer<lmsc::GenerationPreviewDialog> preview = new lmsc::GenerationPreviewDialog(draft, 6, nullptr);
    preview->show();
    QTest::qWait(50);
    const QString captureDirectory = qEnvironmentVariable("LMSC_AI_CAPTURE_DIRECTORY");
    if (!captureDirectory.isEmpty()) {
        QVERIFY2(QDir().mkpath(captureDirectory), qPrintable(captureDirectory));
        struct RestoreTheme {
            QString mode = lmsc::ThemeManager::mode();
            ~RestoreTheme() { lmsc::ThemeManager::apply(mode); }
        } restoreTheme;
        auto captureDraft = draft;
        auto blue = note;
        blue.beat = 3; blue.x = 2; blue.color = 1; blue.direction = 0;
        captureDraft.objects.append(blue);
        auto dot = note;
        dot.beat = 4; dot.direction = 8;
        captureDraft.objects.append(dot);
        lmsc::BeatObject bomb;
        bomb.kind = lmsc::ObjectKind::Bomb; bomb.beat = 6; bomb.x = 3;
        captureDraft.objects.append(bomb);
        lmsc::BeatObject wall;
        wall.kind = lmsc::ObjectKind::Wall; wall.beat = 8; wall.x = 0;
        wall.y = 0; wall.height = 5; wall.width = 1; wall.duration = 2;
        captureDraft.objects.append(wall);
        captureDraft.metrics.directional = 2;
        captureDraft.metrics.dots = 1; captureDraft.metrics.bombs = 1; captureDraft.metrics.walls = 1;
        captureDraft.metrics.averageNps = 0.19; captureDraft.metrics.peakNps = 2.0;
        captureDraft.warnings = QStringList{QStringLiteral("此合成候选仅用于本地界面布局核对。")};
        QPointer<lmsc::GenerationPreviewDialog> capture = new lmsc::GenerationPreviewDialog(captureDraft, 6, nullptr);
        capture->show();
        QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));
        scale.replace('.', '_');
        for (const auto &mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
            lmsc::ThemeManager::apply(mode);
            for (const QSize size : {QSize(1080, 760), QSize(1050, 660)}) {
                capture->resize(size);
                QTest::qWait(80);
                const QString name = QStringLiteral("ai-candidate-preview-%1-scale-%2-%3x%4.png")
                    .arg(mode, scale).arg(size.width()).arg(size.height());
                const QString path = QDir(captureDirectory).filePath(name);
                const auto frame = capture->grab();
                QVERIFY2(frame.save(path), qPrintable(path));
                qInfo().noquote() << QStringLiteral("候选预览截图：%1；窗口 %2x%3，图片 %4x%5")
                    .arg(path).arg(capture->width()).arg(capture->height()).arg(frame.width()).arg(frame.height());
            }
        }
        capture->close();
        QTRY_VERIFY(capture.isNull());
    }
    auto timeline = preview->findChild<TimelineView *>(QStringLiteral("generationPreviewTimeline"));
    auto apply = preview->findChild<QPushButton *>(QStringLiteral("generationPreviewApply"));
    auto cancel = preview->findChild<QPushButton *>(QStringLiteral("generationPreviewCancel"));
    auto regenerate = preview->findChild<QPushButton *>(QStringLiteral("generationPreviewRegenerate"));
    auto notice = preview->findChild<QLabel *>(QStringLiteral("generationReplaceNotice"));
    QVERIFY(timeline && apply && cancel && regenerate && notice);
    QVERIFY(notice->text().contains(QStringLiteral("6 个物件")));
    QSignalSpy moves(timeline, &TimelineView::objectsMoveRequested);
    QSignalSpy deletes(timeline, &TimelineView::deleteRequested);
    QSignalSpy applications(preview, &lmsc::GenerationPreviewDialog::applyRequested);
    QSignalSpy regenerations(preview, &lmsc::GenerationPreviewDialog::regenerateRequested);
    const double row = (timeline->height() - 20 - 88) / 4.0;
    const QPoint position(52 + qRound(1.25 * 90), 88 + qRound(row / 2));
    QTest::mousePress(timeline, Qt::LeftButton, Qt::NoModifier, position);
    QTest::mouseMove(timeline, position + QPoint(60, 0));
    QTest::mouseRelease(timeline, Qt::LeftButton, Qt::NoModifier, position + QPoint(60, 0));
    QTest::keyClick(timeline, Qt::Key_Delete);
    QCOMPARE(moves.count(), 0);
    QCOMPARE(deletes.count(), 0);
    QCOMPARE(preview->draft().objects.first().beat, 2.0);
    apply->click();
    QCOMPARE(applications.count(), 1);
    QVERIFY(preview && preview->isVisible());
    regenerate->click();
    QCOMPARE(regenerations.count(), 1);
    cancel->click();
    QTRY_VERIFY(preview.isNull());
}

void AiGenerationPageTest::busyAndNavigationPreserveProgressAndReusableDraft() {
    FakeGenerationService service; lmsc::AiRecognitionPage page; setup(page,service);
    auto generate=page.findChild<QPushButton *>("aiGenerateButton"); generate->click();
    const auto source=service.requests.last();
    page.hide(); page.setGenerationContext(context(),true,true);
    QVERIFY(service.cancelledJobs.isEmpty()); QVERIFY(page.isRecognizing());
    service.finish(source); QVERIFY(!page.isRecognizing());
    page.setGenerationContext(context(),true,false); page.show();
    auto preview=page.findChild<QPushButton *>("aiViewCandidate"); QVERIFY(preview && preview->isEnabled());
    QSignalSpy ready(&page,&lmsc::AiRecognitionPage::generationDraftReady);
    preview->click(); preview->click(); QCOMPARE(ready.count(),2); QCOMPARE(service.requests.size(),1);
    QCOMPARE(page.findChild<QProgressBar *>("aiRecognitionProgress")->value(),100);
    page.invalidateGeneration(); QVERIFY(!preview->isEnabled());
}
void AiGenerationPageTest::pausedJobCanResumeWithoutStartingOver() {
    FakeGenerationService service; lmsc::AiRecognitionPage page; setup(page,service);
    page.findChild<QPushButton *>("aiGenerateButton")->click(); const auto source=service.requests.last();
    page.cancelRecognition(); QVERIFY(!page.isRecognizing());
    auto resume=page.findChild<QPushButton *>("aiResumeGeneration"); QVERIFY(resume && resume->isEnabled());
    const QString captures=qEnvironmentVariable("LMSC_AI_DIAGNOSTICS_CAPTURE_DIRECTORY");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures)); page.resize(1050,660); page.show();
        for (const auto &mode : {QString("dark"),QString("light")}) {
            lmsc::ThemeManager::apply(mode); QTest::qWait(80);
            QVERIFY(page.grab().save(QDir(captures).filePath("ai-paused-"+mode+".png")));
            page.findChild<QPushButton *>("aiViewLog")->click();
            auto viewer=page.findChild<QDialog *>("diagnosticLogDialog"); QVERIFY(viewer);
            QTest::qWait(80); QVERIFY(viewer->grab().save(QDir(captures).filePath("ai-log-"+mode+".png")));
            viewer->close();
        }
    }
    resume->click(); QCOMPARE(service.resumedJobs,QStringList{source.jobId}); QCOMPARE(service.requests.size(),1);
    QVERIFY(page.isRecognizing()); service.finish(source); QVERIFY(!page.isRecognizing());
}
void AiGenerationPageTest::themeWarningsSurviveNavigationAndPreview() {
    FakeGenerationService service; lmsc::AiRecognitionPage page; setup(page,service);
    page.findChild<QPushButton *>("aiGenerateButton")->click(); const auto source=service.requests.last();
    page.hide(); service.finish(source,QStringLiteral("测试规划"),true);
    page.show(); page.setGenerationContext(context(),true,false);
    auto status=page.findChild<QLabel *>("aiRecognitionStatus");
    QCOMPARE(status->text(),QStringLiteral("候选谱已生成，部分乐句建议试听"));
    QCOMPARE(status->property("role").toString(),QString("warning"));
    QVERIFY(page.findChild<QPlainTextEdit *>("aiRecognitionResult")->toPlainText().contains(QStringLiteral("乐句 7")));
    QSignalSpy drafts(&page,&lmsc::AiRecognitionPage::generationDraftReady);
    page.findChild<QPushButton *>("aiViewCandidate")->click(); QCOMPARE(drafts.count(),1);
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(drafts.first().first()); QVERIFY(draft.hasThemeWarnings);
    QPointer<lmsc::GenerationPreviewDialog> preview=new lmsc::GenerationPreviewDialog(draft,0,nullptr);
    preview->show(); auto warnings=preview->findChild<QLabel *>("generationPreviewWarnings");
    QVERIFY(warnings && warnings->text().contains(QStringLiteral("乐句 7")));
    const QString captures=qEnvironmentVariable("LMSC_AI_THEME_CAPTURE_DIRECTORY");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures)); const QString original=lmsc::ThemeManager::mode();
        page.resize(1050,660); preview->resize(1080,760);
        for (const QString mode : {QString("dark"),QString("light")}) {
            lmsc::ThemeManager::apply(mode); QTest::qWait(80);
            QVERIFY(page.grab().save(QDir(captures).filePath("ai-theme-warning-"+mode+".png")));
            QVERIFY(preview->grab().save(QDir(captures).filePath("ai-preview-warning-"+mode+".png")));
        }
        lmsc::ThemeManager::apply(original);
    }
    preview->close(); QTRY_VERIFY(preview.isNull());
}
void AiGenerationPageTest::automaticRecoveryStaysBusyAcrossNavigation() {
    FakeGenerationService service; lmsc::AiRecognitionPage page; setup(page,service);
    auto *generate=page.findChild<QPushButton *>("aiGenerateButton"); generate->click();
    const auto source=service.requests.last(); service.current.recovering=true; service.current.percent=41;
    emit service.progress(source.jobId,41,QStringLiteral("连接暂时中断 · 正在自动恢复 1/2 · 已完成 6/21 个乐句"));
    QVERIFY(page.isRecognizing()); QVERIFY(!generate->isEnabled());
    QVERIFY(!page.findChild<QPushButton *>("aiResumeGeneration")->isVisible());
    page.hide(); page.show(); page.setGenerationContext(context(),true,false);
    QCOMPARE(page.findChild<QProgressBar *>("aiRecognitionProgress")->value(),41);
    QVERIFY(page.findChild<QLabel *>("aiRecognitionStatus")->text().contains(QStringLiteral("自动恢复")));
    const QString captures=qEnvironmentVariable("LMSC_AI_RECOVERY_CAPTURE_DIRECTORY");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures)); const QString original=lmsc::ThemeManager::mode(); page.resize(1050,660);
        for (const QString mode : {QString("dark"),QString("light")}) {
            lmsc::ThemeManager::apply(mode); QTest::qWait(80);
            QVERIFY(page.grab().save(QDir(captures).filePath("ai-recovering-"+mode+".png")));
        }
        lmsc::ThemeManager::apply(original);
    }
    QVERIFY(page.isRecognizing()); service.current.recovering=false; service.finish(source);
    QVERIFY(!page.isRecognizing()); QCOMPARE(service.requests.size(),1);
}

void AiGenerationPageTest::localModeIsDefaultAndDoesNotRequireModelConnection() {
    FakeGenerationService local;
    lmsc::AiRecognitionPage page;
    auto *mode = page.findChild<QComboBox *>(QStringLiteral("aiGenerationMode"));
    QVERIFY(mode);
    QCOMPARE(mode->count(), 4);
    QCOMPARE(mode->currentData().toInt(), int(lmsc::AiRecognitionPage::Hybrid));
    page.setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    QCOMPARE(page.generationMode(), lmsc::AiRecognitionPage::LocalQuick);
    page.setLocalGenerationService(&local);
    page.setContext(m_audio, QStringLiteral("本地生成测试"), 120, 0.25, 16, false);
    page.setGenerationContext(context(), true, false);
    auto *generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto *analyze = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto *configure = page.findChild<QPushButton *>(QStringLiteral("aiConfigureConnection"));
    QVERIFY(generate->isEnabled());
    QVERIFY(analyze->isEnabled());
    QVERIFY(configure->text().contains(QStringLiteral("可选")));
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("aiServiceStatus"))->text().contains(QStringLiteral("无需配置")));
    analyze->click();
    QCOMPARE(local.requests.size(), 1);
    QVERIFY(local.requests.last().analysisOnly);
    local.finish(local.requests.last(), QStringLiteral("本地分析结果"));
    QSignalSpy drafts(&page, &lmsc::AiRecognitionPage::generationDraftReady);
    generate->click();
    QCOMPARE(local.requests.size(), 2);
    QVERIFY(!local.requests.last().analysisOnly);
    QVERIFY(configure->isEnabled());
    page.cancelRecognition();
    QVERIFY(!page.isRecognizing());
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("aiResumeGeneration"))->isHidden());
    generate->click();
    local.finish(local.requests.last(), QStringLiteral("本地候选谱"));
    QCOMPARE(drafts.count(), 1);
    auto *preview = page.findChild<QPushButton *>(QStringLiteral("aiViewCandidate"));
    QVERIFY(preview->isEnabled());
    preview->click();
    QCOMPARE(drafts.count(), 2);
    page.setGenerationContext(context(), false, false);
    QVERIFY(!generate->isEnabled());
    QVERIFY(analyze->isEnabled());
    page.setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    QVERIFY(!generate->isEnabled());
    QVERIFY(!analyze->isEnabled());
}

void AiGenerationPageTest::switchingModesCancelsAndRejectsOldResults() {
    FakeGenerationService local, model;
    lmsc::AiRecognitionPage page;
    page.setLocalGenerationService(&local);
    page.setGenerationService(&model);
    page.setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    page.setContext(m_audio, {}, 120, 0.25, 16, false);
    page.setGenerationContext(context(), true, false);
    auto *generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto *result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    auto *mode = page.findChild<QComboBox *>(QStringLiteral("aiGenerationMode"));
    QSignalSpy drafts(&page, &lmsc::AiRecognitionPage::generationDraftReady);
    QSignalSpy invalidated(&page, &lmsc::AiRecognitionPage::generationInvalidated);
    generate->click();
    const auto oldLocal = local.requests.last();
    QVERIFY(mode->isEnabled());
    page.setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    QVERIFY(local.cancelledJobs.contains(oldLocal.jobId));
    QVERIFY(!page.isRecognizing());
    local.finish(oldLocal, QStringLiteral("过期本地结果"));
    QCOMPARE(drafts.count(), 0);
    QVERIFY(result->toPlainText().isEmpty());
    generate->click();
    const auto oldModel = model.requests.last();
    local.finish(oldLocal);
    QVERIFY(page.isRecognizing());
    model.finish(oldModel, QStringLiteral("模型候选谱"));
    QCOMPARE(drafts.count(), 1);
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("aiViewCandidate"))->isEnabled());
    const int previousInvalidations = invalidated.count();
    page.setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    QVERIFY(invalidated.count() > previousInvalidations);
    QVERIFY(!page.findChild<QPushButton *>(QStringLiteral("aiViewCandidate"))->isEnabled());
    QVERIFY(result->toPlainText().isEmpty());
    generate->click();
    const auto currentLocal = local.requests.last();
    emit model.requestFailed(oldModel.jobId, QStringLiteral("过期模型错误"));
    emit model.progress(oldModel.jobId, 90, QStringLiteral("过期模型进度"));
    model.finish(oldModel);
    QCOMPARE(drafts.count(), 1);
    QVERIFY(page.isRecognizing());
    local.finish(currentLocal);
    QCOMPARE(drafts.count(), 2);
}

void AiGenerationPageTest::modelChangesPreserveLocalTasksAndDrafts() {
    FakeGenerationService local, model, replacement;
    lmsc::AiRecognitionPage page;
    page.setLocalGenerationService(&local);
    page.setGenerationService(&model);
    page.setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    page.setContext(m_audio, {}, 120, 0.25, 16, false);
    page.setGenerationContext(context(), true, false);
    auto *generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto *result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    auto *status = page.findChild<QLabel *>(QStringLiteral("aiRecognitionStatus"));
    QSignalSpy invalidated(&page, &lmsc::AiRecognitionPage::generationInvalidated);
    generate->click();
    const auto source = local.requests.last();
    emit local.progress(source.jobId, 42, QStringLiteral("本地正在编排"));
    const int previousInvalidations = invalidated.count();
    page.pauseGenerationForConnectionChange();
    page.invalidateGenerationForConnectionChange();
    page.setGenerationService(&replacement);
    model.available = false;
    emit model.availabilityChanged();
    replacement.available = false;
    emit replacement.availabilityChanged();
    QVERIFY(page.isRecognizing());
    QVERIFY(local.cancelledJobs.isEmpty());
    QCOMPARE(invalidated.count(), previousInvalidations);
    QCOMPARE(status->text(), QStringLiteral("本地正在编排"));
    QCOMPARE(page.findChild<QProgressBar *>(QStringLiteral("aiRecognitionProgress"))->value(), 42);
    local.finish(source, QStringLiteral("保留的本地候选"));
    page.pauseGenerationForConnectionChange();
    page.invalidateGenerationForConnectionChange();
    page.setGenerationService(&model);
    QCOMPARE(invalidated.count(), previousInvalidations);
    QVERIFY(result->toPlainText().contains(QStringLiteral("保留的本地候选")));
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("aiViewCandidate"))->isEnabled());
    page.setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    QVERIFY(!generate->isEnabled());
}

void AiGenerationPageTest::inactiveBackendDestructionAndSharedBackendsAreSafe() {
    FakeGenerationService local;
    auto *model = new FakeGenerationService;
    lmsc::AiRecognitionPage page;
    page.setLocalGenerationService(&local);
    page.setGenerationService(model);
    page.setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    page.setContext(m_audio, {}, 120, 0.25, 16, false);
    page.setGenerationContext(context(), true, false);
    auto *generate = page.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    generate->click();
    const auto source = local.requests.last();
    delete model;
    QVERIFY(page.isRecognizing());
    QVERIFY(local.cancelledJobs.isEmpty());
    local.finish(source);
    page.setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    QVERIFY(!generate->isEnabled());
    page.setGenerationService(&local);
    generate->click();
    const auto oldRequest = local.requests.last();
    page.setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    QVERIFY(local.cancelledJobs.contains(oldRequest.jobId));
    QSignalSpy drafts(&page, &lmsc::AiRecognitionPage::generationDraftReady);
    generate->click();
    const auto newRequest = local.requests.last();
    local.finish(oldRequest);
    QCOMPARE(drafts.count(), 0);
    QVERIFY(page.isRecognizing());
    local.finish(newRequest);
    QCOMPARE(drafts.count(), 1);
    FakeGenerationService fallback;
    auto *injected = new FakeGenerationService;
    page.setLocalGenerationService(injected, &fallback);
    generate->click();
    delete injected;
    QVERIFY(!page.isRecognizing());
    QCOMPARE(page.generationMode(), lmsc::AiRecognitionPage::LocalQuick);
    QVERIFY(generate->isEnabled());
    generate->click();
    QCOMPARE(fallback.requests.size(), 1);
}
void AiGenerationPageTest::hybridModeDefaultsToPlanningAndChangesSeed() {
    FakeGenerationService hybrid, model;
    lmsc::AiRecognitionPage page;
    QCOMPARE(page.generationMode(), lmsc::AiRecognitionPage::Hybrid);
    page.setHybridGenerationService(&hybrid);
    page.setGenerationService(&model);
    page.setContext(m_audio, QStringLiteral("混合编排"), 120, .25, 16, false);
    page.setGenerationContext(context(), true, false);
    auto generate=page.findChild<QPushButton *>("aiGenerateButton");
    auto skip=page.findChild<QPushButton *>("aiSkipPlanning");
    auto variation=page.findChild<QPushButton *>("aiChangeArrangement");
    QSignalSpy skipped(&page,&lmsc::AiRecognitionPage::skipPlanningRequested);
    QSignalSpy drafts(&page,&lmsc::AiRecognitionPage::generationDraftReady);
    generate->click(); QCOMPARE(hybrid.requests.size(),1); QVERIFY(model.requests.isEmpty());
    QCOMPARE(hybrid.requests.last().arrangementSeed,quint32(0)); QVERIFY(skip->isEnabled());
    skip->click(); QCOMPARE(skipped.count(),1); QCOMPARE(skipped.first().first().toString(),hybrid.requests.last().jobId);
    hybrid.finish(hybrid.requests.last(),QStringLiteral("有效初稿"));
    page.invalidateGenerationForConnectionChange();
    QVERIFY(page.findChild<QPushButton *>("aiViewCandidate")->isEnabled());
    QVERIFY(page.findChild<QPlainTextEdit *>("aiRecognitionResult")->toPlainText().contains(QStringLiteral("有效初稿")));
    QVERIFY(variation->isEnabled()); variation->click(); QCOMPARE(hybrid.requests.size(),2);
    QCOMPARE(hybrid.requests.last().arrangementSeed,quint32(1));
    const auto late=hybrid.requests.last(); page.invalidateGenerationForConnectionChange();
    hybrid.finish(late,QStringLiteral("过期模型编排")); QCOMPARE(drafts.count(),1);
}

void AiGenerationPageTest::infernoModeRestrictsTypesAndRestoresOtherOptions() {
    FakeGenerationService local, inferno;
    lmsc::AiRecognitionPage page;
    page.setLocalGenerationService(&local);
    page.setInfernoGenerationService(&inferno);
    page.setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    page.setContext(m_audio, {}, 120, .25, 16, false);
    page.setGenerationContext(context(), true, false);
    auto difficulty = page.findChild<QComboBox *>("aiGenerationDifficulty");
    auto directional = page.findChild<QCheckBox *>("aiDirectionalType");
    auto dots = page.findChild<QCheckBox *>("aiDotType");
    auto bombs = page.findChild<QCheckBox *>("aiBombType");
    auto walls = page.findChild<QCheckBox *>("aiWallType");
    auto generate = page.findChild<QPushButton *>("aiGenerateButton");
    auto analyze = page.findChild<QPushButton *>("aiRecognizeButton");
    difficulty->setCurrentIndex(difficulty->findData(QStringLiteral("Normal")));
    directional->setChecked(false); bombs->setChecked(true); walls->setChecked(true);
    page.setGenerationMode(lmsc::AiRecognitionPage::InfernoSaber);
    QCOMPARE(difficulty->currentData().toString(), QStringLiteral("Expert"));
    auto model = qobject_cast<QStandardItemModel *>(difficulty->model()); QVERIFY(model);
    for (int i = 0; i < difficulty->count(); ++i)
        QCOMPARE(model->item(i)->isEnabled(), difficulty->itemData(i).toString() == "Hard" || difficulty->itemData(i).toString() == "Expert");
    QVERIFY(directional->isChecked() && dots->isChecked());
    QVERIFY(!bombs->isChecked() && !walls->isChecked());
    for (auto box : {directional, dots, bombs, walls}) QVERIFY(!box->isEnabled());
    QVERIFY(page.findChild<QLabel *>("aiServiceStatus")->text().contains(QStringLiteral("CPU")));
    QCOMPARE(page.findChild<QPushButton *>("aiConfigureConnection")->text(), QStringLiteral("配置本地模型"));
    difficulty->setCurrentIndex(difficulty->findData(QStringLiteral("Hard")));
    generate->click(); QCOMPARE(inferno.requests.size(), 1); QVERIFY(local.requests.isEmpty());
    QCOMPARE(inferno.requests.last().profile.name, QStringLiteral("Hard"));
    QCOMPARE(inferno.requests.last().allowedTypes, lmsc::GeneratedTypes(lmsc::DirectionalType | lmsc::DotType));
    inferno.finish(inferno.requests.last());
    page.findChild<QPushButton *>("aiChangeArrangement")->click();
    QCOMPARE(inferno.requests.size(), 2); QCOMPARE(inferno.requests.last().arrangementSeed, quint32(1));
    page.cancelRecognition();
    inferno.available = false; emit inferno.availabilityChanged();
    QVERIFY(!generate->isEnabled()); QVERIFY(analyze->isEnabled());
    analyze->click(); QCOMPARE(inferno.requests.size(), 3); QVERIFY(inferno.requests.last().analysisOnly);
    inferno.finish(inferno.requests.last());
    page.setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    QCOMPARE(difficulty->currentData().toString(), QStringLiteral("Normal"));
    QVERIFY(!directional->isChecked() && !dots->isChecked()); QVERIFY(bombs->isChecked() && walls->isChecked());
    for (int i = 0; i < difficulty->count(); ++i) QVERIFY(model->item(i)->isEnabled());
}

void AiGenerationPageTest::infernoTasksIgnoreAiConnectionsAndRejectCancelledResults() {
    FakeGenerationService inferno, model;
    lmsc::AiRecognitionPage page;
    page.setInfernoGenerationService(&inferno); page.setGenerationService(&model);
    page.setGenerationMode(lmsc::AiRecognitionPage::InfernoSaber);
    page.setContext(m_audio, {}, 120, .25, 16, false); page.setGenerationContext(context(), true, false);
    auto generate = page.findChild<QPushButton *>("aiGenerateButton");
    QSignalSpy drafts(&page, &lmsc::AiRecognitionPage::generationDraftReady);
    generate->click(); const auto original = inferno.requests.last();
    page.pauseGenerationForConnectionChange(); page.invalidateGenerationForConnectionChange();
    page.setGenerationService(nullptr); QVERIFY(page.isRecognizing()); QVERIFY(inferno.cancelledJobs.isEmpty());
    emit inferno.progress(original.jobId, 71, QStringLiteral("正在运行本地模型"));
    QCOMPARE(page.findChild<QProgressBar *>("aiRecognitionProgress")->value(), 71);
    inferno.finish(original); QCOMPARE(drafts.count(), 1);
    generate->click(); const auto cancelled = inferno.requests.last(); page.cancelRecognition();
    inferno.finish(cancelled); QCOMPARE(drafts.count(), 1);
    generate->click(); const auto stale = inferno.requests.last();
    auto changed = context(); QVERIFY(changed.timeMap.configure(121, .25)); page.setGenerationContext(changed, true, false);
    QVERIFY(inferno.cancelledJobs.contains(stale.jobId)); inferno.finish(stale); QCOMPARE(drafts.count(), 1);
    generate->click(); const auto switched = inferno.requests.last();
    page.setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    QVERIFY(inferno.cancelledJobs.contains(switched.jobId)); inferno.finish(switched); QCOMPARE(drafts.count(), 1);
}

void AiGenerationPageTest::refinementPreviewComparesRestoresAndPreservesIds() {
    FakeRefinementService service;
    lmsc::GenerationDraft initial; initial.source=context(); initial.source.jobId="initial";
    lmsc::BeatObject a; a.beat=2; a.x=0; a.y=1; a.direction=1;
    lmsc::BeatObject b=a; b.id="candidate:0"; b.beat=4; b.color=1; b.x=3; b.direction=0;
    initial.objects={a,b};
    auto preview=new lmsc::GenerationPreviewDialog(initial,0,nullptr);
    QPointer<lmsc::GenerationPreviewDialog> guard(preview);
    preview->setRefinementService(&service); preview->resize(1080,760); preview->show();
    QCOMPARE(preview->initialDraft().objects.at(1).id,QStringLiteral("candidate:0"));
    QVERIFY(!preview->initialDraft().objects.at(0).id.isEmpty());
    QVERIFY(preview->initialDraft().objects.at(0).id!=preview->initialDraft().objects.at(1).id);
    preview->setRefinementSelection(2,10); preview->refineChart(); QCOMPARE(service.requests.size(),1);
    const auto request=service.requests.last(); QVERIFY(request.selectedOnly);
    QCOMPARE(request.startSeconds,2.0); QCOMPARE(request.endSeconds,10.0); QCOMPARE(request.maximumSegments,5);
    QCOMPARE(request.baselineHash,lmsc::refinementBaselineHash(preview->initialDraft().objects));
    QVERIFY(!preview->findChild<QPushButton *>("generationPreviewApply")->isEnabled());
    service.finish(request);
    QVERIFY(preview->hasRefinementPatch()); QCOMPARE(preview->draft().objects.first().y,2);
    QVERIFY(preview->findChild<QLabel *>("generationRefinementStats")->text().contains(QStringLiteral("剩余 1")));
    QVERIFY(preview->findChild<QLabel *>("generationRefinementStats")->text().contains(QStringLiteral("s1 · 2.000–8.000 秒")));
    QTest::qWait(20); QVERIFY(preview->minimumSizeHint().height()<=760);
    auto compare=preview->findChild<QComboBox *>("generationPreviewComparison");
    compare->setCurrentIndex(0); QVERIFY(!preview->hasRefinementPatch()); QCOMPARE(preview->draft().objects.first().y,1);
    compare->setCurrentIndex(1); QCOMPARE(preview->draft().objects.first().y,2);
    const QString captures=qEnvironmentVariable("LMSC_HYBRID_CAPTURE_DIRECTORY");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures)); const auto mode=lmsc::ThemeManager::mode();
        for (const QString theme : {QStringLiteral("light"),QStringLiteral("dark")}) {
            lmsc::ThemeManager::apply(theme); QTest::qWait(100);
            QVERIFY(preview->grab().save(QDir(captures).filePath("refinement-preview-"+theme+".png")));
        }
        lmsc::ThemeManager::apply(mode);
    }
    preview->restoreInitialDraft(); QVERIFY(!preview->hasRefinementPatch()); QCOMPARE(preview->draft().objects.first().y,1);
    QVERIFY(service.discardedJobs.contains(request.generation.jobId));
    preview->close(); QTRY_VERIFY(guard.isNull());
}

void AiGenerationPageTest::refinementCancelSettingsAndResumeRejectLateResults() {
    FakeRefinementService service;
    lmsc::GenerationDraft initial; initial.source=context(); initial.source.jobId="initial";
    lmsc::BeatObject note; note.beat=2; note.x=0; note.y=1; note.direction=1; initial.objects={note};
    auto preview=new lmsc::GenerationPreviewDialog(initial,1,nullptr);
    QPointer<lmsc::GenerationPreviewDialog> guard(preview);
    preview->setRefinementService(&service); QSignalSpy finished(preview,&lmsc::GenerationPreviewDialog::refinementCompleted);
    preview->refineChart(); const auto cancelled=service.requests.last(); preview->cancelRefinement();
    service.finish(cancelled); QCOMPARE(finished.count(),0); QVERIFY(!preview->hasRefinementPatch());
    preview->refineChart(); const auto request=service.requests.last(); service.finish(request);
    QCOMPARE(finished.count(),1); QVERIFY(preview->hasRefinementPatch());
    emit service.requestFailed(request.generation.jobId,QStringLiteral("本轮请求超时"));
    QVERIFY(preview->findChild<QLabel *>("generationPreviewStatus")->text().contains(QStringLiteral("本轮请求超时")));
    QVERIFY(preview->hasRefinementPatch());
    preview->resumeRefinement(); QCOMPARE(service.resumedJobs,QStringList{request.generation.jobId});
    QVERIFY(preview->isRefining()); service.finish(request,false); QCOMPARE(finished.count(),2);
    preview->refineChart(); const auto stale=service.requests.last();
    preview->invalidateRefinementForConnectionChange(); QVERIFY(!preview->hasRefinementPatch());
    service.finish(stale); QCOMPARE(finished.count(),2); QCOMPARE(preview->draft().objects.first().y,1);
    preview->refineChart(); auto altered=service.result(service.requests.last()); ++altered.source.generation.documentRevision;
    emit service.candidateReady(altered); QCOMPARE(finished.count(),2); QVERIFY(preview->isRefining());
    preview->cancelRefinement(); preview->close(); QTRY_VERIFY(guard.isNull());
}

void AiGenerationPageTest::refinementCallbacksMayDestroyPreview() {
    FakeRefinementService service;
    lmsc::GenerationDraft initial; initial.source=context(); initial.source.jobId="initial";
    lmsc::BeatObject note; note.beat=2; note.x=0; note.y=1; note.direction=1; initial.objects={note};
    auto preview=new lmsc::GenerationPreviewDialog(initial,1,nullptr);
    QPointer<lmsc::GenerationPreviewDialog> guard(preview); preview->setRefinementService(&service);
    connect(preview,&lmsc::GenerationPreviewDialog::refinementCompleted,preview,[preview] { delete preview; });
    preview->refineChart(); service.finish(service.requests.last()); QVERIFY(guard.isNull());
    auto cancelledPreview=new lmsc::GenerationPreviewDialog(initial,1,nullptr);
    guard=cancelledPreview; cancelledPreview->setRefinementService(&service); cancelledPreview->refineChart();
    connect(cancelledPreview,&lmsc::GenerationPreviewDialog::cancelRefinementRequested,cancelledPreview,[cancelledPreview] { delete cancelledPreview; });
    cancelledPreview->cancelRefinement(); QVERIFY(guard.isNull());
    auto validatedPreview=new lmsc::GenerationPreviewDialog(initial,1,nullptr);
    guard=validatedPreview;
    validatedPreview->setSourceValidation([validatedPreview](const lmsc::GenerationRequest &) { delete validatedPreview; return true; });
    QVERIFY(guard.isNull());
}
void AiGenerationPageTest::refinementRejectsAlteredSource_data() {
    QTest::addColumn<int>("change");
    struct Case { const char *name; int change; };
    const Case cases[] = {{"same-count-different-bpm",0},{"same-count-different-beat",1},
        {"different-pcm-rate",2},{"different-pcm-channels",3},{"different-hand-constraint",8},
        {"analysis-only",11},{"different-arrangement",12},{"separate-candidate-source",13},
        {"cached-result-different-seed",16},{"cached-result-different-types",17},{"cached-result-different-difficulty",18}};
    for (const auto &item : cases) QTest::newRow(item.name) << item.change;
}

void AiGenerationPageTest::refinementRejectsAlteredSource() {
    QFETCH(int, change);
    FakeRefinementService service;
    lmsc::GenerationDraft initial; initial.source=context(); initial.source.jobId="initial";
    QVERIFY(initial.source.timeMap.configure(120,.25,{{8,90},{16,130}}));
    auto arrangement=std::make_shared<lmsc::SongArrangementPlan>(); arrangement->summary=QStringLiteral("原规划");
    initial.source.arrangement=arrangement; initial.arrangement=arrangement;
    lmsc::BeatObject note; note.beat=2; note.x=0; note.y=1; note.direction=1; initial.objects={note};
    auto preview=new lmsc::GenerationPreviewDialog(initial,1,nullptr);
    QPointer<lmsc::GenerationPreviewDialog> guard(preview); preview->setRefinementService(&service);
    QSignalSpy finished(preview,&lmsc::GenerationPreviewDialog::refinementCompleted);
    preview->refineChart(); const auto request=service.requests.last(); auto altered=service.result(request);
    auto &source=altered.source.generation;
    switch (change) {
    case 0: QVERIFY(source.timeMap.configure(120,.25,{{8,100},{16,130}})); break;
    case 1: QVERIFY(source.timeMap.configure(120,.25,{{9,90},{16,130}})); break;
    case 2: ++source.audio.sampleRate; break;
    case 3: source.audio.channels=1; break;
    case 8: source.profile.minSameHandGapSeconds+=.01; break;
    case 11: source.analysisOnly=true; break;
    case 12: { auto different=std::make_shared<lmsc::SongArrangementPlan>(*source.arrangement); different->summary=QStringLiteral("另一规划"); source.arrangement=different; break; }
    case 13: altered.candidate.source.documentId=QStringLiteral("another-document"); break;
    case 16: ++source.arrangementSeed; break;
    case 17: source.allowedTypes=lmsc::DotType; break;
    case 18: source.profile=lmsc::DifficultyProfile::forName("Hard"); break;
    }
    if (change!=13) altered.candidate.source=source;
    // Equal job IDs and baseline hashes must not allow a changed tempo, PCM,
    // difficulty constraint, or separately substituted candidate source.
    emit service.candidateReady(altered);
    QCOMPARE(finished.count(),0); QVERIFY(preview->isRefining()); QVERIFY(!preview->hasRefinementPatch());
    QCOMPARE(preview->draft().objects.first().y,1);
    preview->setRefinementResult(altered); QVERIFY(!preview->hasRefinementPatch());
    service.finish(request,false); QCOMPARE(finished.count(),1); QVERIFY(preview->hasRefinementPatch());
    preview->close(); QTRY_VERIFY(guard.isNull());
}
QTEST_MAIN(AiGenerationPageTest)
#include "AiGenerationPageTest.moc"
