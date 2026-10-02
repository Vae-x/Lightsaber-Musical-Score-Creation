#include "core/AiGenerationService.h"
#include "gui/AiRecognitionPage.h"
#include "gui/EditorViews.h"
#include "gui/GenerationPreviewDialog.h"
#include "gui/ThemeManager.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>

namespace {
class FakeGenerationService final : public lmsc::AiGenerationService {
public:
    bool available = true;
    QVector<lmsc::GenerationRequest> requests;
    QStringList cancelledJobs;
    bool isAvailable() const override { return available; }
    void generate(const lmsc::GenerationRequest &request) override { requests.append(request); }
    void cancel(const QString &jobId) override { cancelledJobs.append(jobId); emit cancelled(jobId); }
    void finish(const lmsc::GenerationRequest &source, const QString &summary = QStringLiteral("测试规划")) {
        lmsc::GenerationDraft draft;
        draft.source = source;
        draft.summary = summary;
        if (!source.analysisOnly) {
            lmsc::BeatObject note;
            note.beat = 2.0; note.x = 0; note.y = 1; note.direction = 1;
            draft.objects.append(note);
            draft.metrics.directional = 1;
        }
        emit draftReady(draft);
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

QTEST_MAIN(AiGenerationPageTest)
#include "AiGenerationPageTest.moc"
