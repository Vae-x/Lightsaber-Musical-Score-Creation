#include "core/LocalAiGenerationService.h"
#include "core/LocalChartGenerator.h"
#include "core/BeatmapPlayabilityValidator.h"

#include <QElapsedTimer>
#include <QFile>
#include <QNetworkProxy>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr int sampleRate = 2000;
constexpr int channels = 2;

// Real PCM, rather than fabricated MusicAnalysis, exercises onset extraction,
// subdivisions, repeated sections and the silent interval together.
bool writeMusic(const QString &path, double duration, const lmsc::TimeMap &map,
                bool silence = false, double spacingBeats = 1.0, int rate = sampleRate) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QVector<double> pulses;
    for (double beat = 0; map.beatToSeconds(beat) < duration; beat += spacingBeats) {
        const double seconds = map.beatToSeconds(beat);
        if (seconds < 0 || (silence && seconds >= 12 && seconds < 16)) continue;
        pulses.append(seconds);
    }
    QByteArray buffer;
    buffer.reserve(65536);
    int pulse = -1;
    const int frames = qRound(duration * rate);
    for (int frame = 0; frame < frames; ++frame) {
        const double time = frame / double(rate);
        while (pulse + 1 < pulses.size() && pulses[pulse + 1] <= time) ++pulse;
        const double age = pulse >= 0 ? time - pulses[pulse] : 1.0;
        const bool quiet = silence && time >= 12 && time < 16;
        const double beat = pulse >= 0 ? map.secondsToBeat(pulses[pulse]) : 0;
        const double accent = qRound64(beat * 2) % 8 == 0 ? 0.82 : 0.55;
        const double envelope = age < 0.06 ? accent * std::exp(-age * 45) : 0;
        const double sample = quiet ? 0 : (0.025 + envelope) * std::sin(2 * pi * 110 * time);
        const qint16 integer = static_cast<qint16>(sample * 30000);
        for (int channel = 0; channel < channels; ++channel) {
            char bytes[2];
            qToLittleEndian<qint16>(integer, reinterpret_cast<uchar *>(bytes));
            buffer.append(bytes, 2);
        }
        if (buffer.size() >= 64000) {
            if (file.write(buffer) != buffer.size()) return false;
            buffer.clear();
        }
    }
    return buffer.isEmpty() || file.write(buffer) == buffer.size();
}

int typeBit(const lmsc::BeatObject &object) {
    return object.kind == lmsc::ObjectKind::Wall ? lmsc::WallType
        : object.kind == lmsc::ObjectKind::Bomb ? lmsc::BombType
        : object.direction == 8 ? lmsc::DotType : lmsc::DirectionalType;
}

QVector<lmsc::BeatObject> sectionNotes(const lmsc::GenerationDraft &draft,
                                    const lmsc::MusicSegment &segment) {
    QVector<lmsc::BeatObject> notes;
    for (const auto &object : draft.objects)
        if (object.kind == lmsc::ObjectKind::Note && object.beat >= segment.startBeat - 1e-7
                && object.beat < segment.endBeat - 1e-7) notes.append(object);
    return notes;
}

void verifyDraft(const lmsc::GenerationRequest &source, const lmsc::MusicAnalysis &analysis,
                 const lmsc::GenerationDraft &draft) {
    QCOMPARE(draft.source.jobId, source.jobId);
    QCOMPARE(draft.source.documentId, source.documentId);
    QCOMPARE(draft.source.documentRevision, source.documentRevision);
    QCOMPARE(draft.source.difficultyId, source.difficultyId);
    QCOMPARE(draft.source.audioRevision, source.audioRevision);
    QCOMPARE(draft.source.audio.revision, source.audio.revision);
    QCOMPARE(draft.source.audio.path, source.audio.path);
    QCOMPARE(draft.source.audio.sourcePath, source.audio.sourcePath);
    QCOMPARE(draft.source.profile.name, source.profile.name);
    QCOMPARE(int(draft.source.allowedTypes), int(source.allowedTypes));
    QCOMPARE(draft.source.timeMap.initialBpm(), source.timeMap.initialBpm());
    QCOMPARE(draft.source.timeMap.offsetSeconds(), source.timeMap.offsetSeconds());
    QVERIFY(!draft.summary.isEmpty());
    QStringList errors;
    QVERIFY2(lmsc::BeatmapPlayabilityValidator::validateObjects(draft.objects, source, analysis, &errors),
             qPrintable(errors.join('\n')));
    QSet<qint64> hitBeats;
    for (const auto &anchor : analysis.anchors)
        if (anchor.kind == lmsc::MusicAnchorKind::Hit) hitBeats.insert(qRound64(anchor.beat * 1000000));
    for (const auto &object : draft.objects) {
        QVERIFY(int(source.allowedTypes) & typeBit(object));
        if (object.kind == lmsc::ObjectKind::Note)
            QVERIFY2(hitBeats.contains(qRound64(object.beat * 1000000)), "击打没有绑定真实音乐起音锚点");
    }
    const auto measured = lmsc::BeatmapPlayabilityValidator::metrics(draft.objects, source.timeMap, analysis.activeSeconds);
    QCOMPARE(draft.metrics.directional, measured.directional);
    QCOMPARE(draft.metrics.dots, measured.dots);
    QCOMPARE(draft.metrics.bombs, measured.bombs);
    QCOMPARE(draft.metrics.walls, measured.walls);
    QVERIFY(std::abs(draft.metrics.averageNps - measured.averageNps) < 1e-8);
    QVERIFY(std::abs(draft.metrics.peakNps - measured.peakNps) < 1e-8);
}

class ProxyGuard {
public:
    ProxyGuard() : previous(QNetworkProxy::applicationProxy()) {
        // Deliberately unusable proxy; this does not touch the user's network.
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1));
    }
    ~ProxyGuard() { QNetworkProxy::setApplicationProxy(previous); }
private:
    QNetworkProxy previous;
};
}

class LocalGenerationTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void everyDifficulty_data();
    void everyDifficulty();
    void options_data();
    void options();
    void slowTempoWallsHaveRealMusicalEvidence();
    void bareBoundariesDoNotCreateWalls();
    void silenceHasNoHits();
    void denseMusicRespectsDifficultyAndHandConnections();
    void repeatsKeepThemesAndSectionBoundariesSafe();
    void variableTempoAndOffsetPreserveSource();
    void analysisOnlyAndInvalidRequests();
    void coreCancellationDoesNotPublishPartialDraft();
    void serviceRunsOfflineAndKeepsGuiResponsive();
    void cacheMatchesSourceAndTiming();
    void cachedAnalysisRequiresExistingAudio();
    void serviceRejectsMismatchedAudioRevision();
    void replacementAndCancellationSuppressOldResults_data();
    void replacementAndCancellationSuppressOldResults();
    void completedCancelledAndDestroyedWorkersReleaseAudioLease();
    void progressReceiverMayDestroyService_data();
    void progressReceiverMayDestroyService();
    void cancellationReceiverMayGenerateReplacement();
    void cancellationReceiverMayDestroyService();
    void threeMinuteBenchmark();
private:
    QTemporaryDir temporary;
    QString music, repeated, dense, longMusic, variable;
    lmsc::TimeMap variableMap;
    lmsc::GenerationRequest request(const QString &path = {}, double duration = 32) const;
};

lmsc::GenerationRequest LocalGenerationTest::request(const QString &path, double duration) const {
    lmsc::GenerationRequest source;
    source.jobId = "local-job";
    source.documentId = "new-song";
    source.difficultyId = "generated-difficulty";
    source.documentRevision = 17;
    source.audioRevision = 5;
    source.profile = lmsc::DifficultyProfile::forName("Expert");
    source.audio.path = path.isEmpty() ? music : path;
    source.audio.sourcePath = "private-local-test-audio.ogg";
    source.audio.revision = 5;
    source.audio.sampleRate = sampleRate;
    source.audio.channels = channels;
    source.audio.durationSeconds = duration;
    source.timeMap.configure(120, 0);
    return source;
}

void LocalGenerationTest::initTestCase() {
    QVERIFY(temporary.isValid());
    lmsc::TimeMap map;
    QVERIFY(map.configure(120, 0));
    music = temporary.filePath("music.pcm");
    repeated = temporary.filePath("repeated.pcm");
    dense = temporary.filePath("dense.pcm");
    longMusic = temporary.filePath("three-minutes.pcm");
    variable = temporary.filePath("variable-tempo.pcm");
    QVERIFY(writeMusic(music, 32, map, true));
    QVERIFY(writeMusic(repeated, 48, map));
    QVERIFY(writeMusic(dense, 32, map, false, 0.25));
    QVERIFY(writeMusic(longMusic, 180, map));
    QVERIFY(variableMap.configure(120, 0.375, {{24, 90}, {48, 150}}));
    QVERIFY(writeMusic(variable, 42, variableMap));
    qRegisterMetaType<lmsc::GenerationDraft>();
}

void LocalGenerationTest::everyDifficulty_data() {
    QTest::addColumn<QString>("difficulty");
    for (const auto &name : QStringList{"Easy", "Normal", "Hard", "Expert", "ExpertPlus"})
        QTest::newRow(qPrintable(name)) << name;
}

void LocalGenerationTest::everyDifficulty() {
    QFETCH(QString, difficulty);
    auto source = request();
    source.profile = lmsc::DifficultyProfile::forName(difficulty);
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    QVector<int> progress;
    QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error, {},
                 [&](int percent) { progress.append(percent); }), qPrintable(error));
    QVERIFY(!draft.objects.isEmpty());
    verifyDraft(source, analysis, draft);
    QVERIFY(draft.metrics.directional > 0);
    QVERIFY(draft.metrics.peakNps <= source.profile.maxPeakNps + 1e-7);
    QVERIFY(draft.metrics.averageNps <= source.profile.targetMaxNps + 1e-7);
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last(), 100);
    for (int i = 1; i < progress.size(); ++i) QVERIFY(progress[i] >= progress[i - 1]);
}

void LocalGenerationTest::options_data() {
    QTest::addColumn<int>("types");
    QTest::newRow("directional") << int(lmsc::DirectionalType);
    QTest::newRow("dot") << int(lmsc::DotType);
    QTest::newRow("bomb") << int(lmsc::BombType);
    QTest::newRow("wall") << int(lmsc::WallType);
    QTest::newRow("all") << 15;
}

void LocalGenerationTest::options() {
    QFETCH(int, types);
    auto source = request();
    source.allowedTypes = lmsc::GeneratedTypes(types);
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error), qPrintable(error));
    QVERIFY(!draft.objects.isEmpty());
    verifyDraft(source, analysis, draft);
    int observedTypes = 0;
    for (const auto &object : draft.objects) observedTypes |= typeBit(object);
    QCOMPARE(observedTypes, types);
}

void LocalGenerationTest::slowTempoWallsHaveRealMusicalEvidence() {
    lmsc::TimeMap slowMap;
    QVERIFY(slowMap.configure(40, 0));
    const QString slowMusic = temporary.filePath("slow-tempo.pcm");
    QVERIFY(writeMusic(slowMusic, 36, slowMap));
    auto source = request(slowMusic, 36);
    source.timeMap = slowMap;
    source.profile = lmsc::DifficultyProfile::forName("Easy");
    source.allowedTypes = lmsc::WallType;
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    QVERIFY(analysis.activeSeconds > 30);
    QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error), qPrintable(error));
    QVERIFY(!draft.objects.isEmpty());
    QVERIFY(draft.metrics.walls > 0);
    for (const auto &object : draft.objects) QCOMPARE(object.kind, lmsc::ObjectKind::Wall);
    verifyDraft(source, analysis, draft);
}

void LocalGenerationTest::bareBoundariesDoNotCreateWalls() {
    auto source = request(repeated, 48);
    source.allowedTypes = lmsc::WallType;
    lmsc::MusicAnalysis original;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &original, &error), qPrintable(error));
    auto evidence = original;
    evidence.anchors.clear();
    evidence.anchorIndex.clear();
    QVector<double> boundaries;
    for (const auto &anchor : original.anchors)
        if (anchor.kind == lmsc::MusicAnchorKind::Boundary) boundaries.append(anchor.seconds);
    for (const auto &anchor : original.anchors) {
        bool nearBoundary = false;
        if (anchor.kind != lmsc::MusicAnchorKind::Boundary)
            for (double seconds : boundaries)
                if (std::abs(anchor.seconds - seconds) < 0.125 - 1e-7) { nearBoundary = true; break; }
        if (nearBoundary) continue;
        evidence.anchorIndex.insert(anchor.id, evidence.anchors.size());
        evidence.anchors.append(anchor);
    }
    int activities = 0;
    double previousActivity = -1;
    for (const auto &anchor : evidence.anchors) {
        if (anchor.kind == lmsc::MusicAnchorKind::Boundary) continue;
        for (double seconds : boundaries) QVERIFY(std::abs(anchor.seconds - seconds) >= 0.125 - 1e-7);
        // Retained energetic rests still describe a continuous musical passage.
        if (previousActivity >= 0) QVERIFY(anchor.seconds - previousActivity <= 1.0 + 1e-7);
        previousActivity = anchor.seconds;
        ++activities;
    }
    QVERIFY(activities > 8);
    for (int segment = 0; segment < evidence.segments.size(); ++segment) {
        auto &indices = evidence.segments[segment].anchors;
        indices.clear();
        for (int originalIndex : original.segments[segment].anchors) {
            const auto found = evidence.anchorIndex.constFind(original.anchors[originalIndex].id);
            if (found != evidence.anchorIndex.constEnd()) indices.append(found.value());
        }
        QVERIFY(indices.size() <= 128);
    }
    lmsc::GenerationDraft draft;
    auto analysisRequest = source;
    analysisRequest.analysisOnly = true;
    QVERIFY2(lmsc::LocalChartGenerator::generate(analysisRequest, evidence, &draft, &error), qPrintable(error));
    QVERIFY(!lmsc::LocalChartGenerator::generate(source, evidence, &draft, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(draft.objects.isEmpty());
}

void LocalGenerationTest::silenceHasNoHits() {
    auto source = request();
    source.profile = lmsc::DifficultyProfile::forName("ExpertPlus");
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error), qPrintable(error));
    bool before = false, after = false;
    for (const auto &object : draft.objects) {
        if (object.kind != lmsc::ObjectKind::Note) continue;
        const double seconds = source.timeMap.beatToSeconds(object.beat);
        QVERIFY2(seconds < 12 || seconds >= 16, "静音区间出现了击打");
        before |= seconds < 12;
        after |= seconds >= 16;
    }
    QVERIFY(before);
    QVERIFY(after);
    verifyDraft(source, analysis, draft);
}

void LocalGenerationTest::denseMusicRespectsDifficultyAndHandConnections() {
    for (const auto &name : QStringList{"Easy", "ExpertPlus"}) {
        auto source = request(dense);
        source.profile = lmsc::DifficultyProfile::forName(name);
        lmsc::MusicAnalysis analysis;
        lmsc::GenerationDraft draft;
        QString error;
        QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
        QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error), qPrintable(error));
        verifyDraft(source, analysis, draft);
        QVector<lmsc::BeatObject> notes = draft.objects;
        std::sort(notes.begin(), notes.end(), [](const lmsc::BeatObject &a, const lmsc::BeatObject &b) {
            return a.beat < b.beat;
        });
        double lastSeconds[2] = {-1e6, -1e6};
        int handCount[2] = {};
        for (const auto &object : notes) {
            if (object.kind != lmsc::ObjectKind::Note) continue;
            const double seconds = source.timeMap.beatToSeconds(object.beat);
            QVERIFY(seconds - lastSeconds[object.color] >= source.profile.minSameHandGapSeconds - 1e-7);
            lastSeconds[object.color] = seconds;
            ++handCount[object.color];
        }
        QVERIFY(handCount[0] > 0);
        QVERIFY(handCount[1] > 0);
        QVERIFY(draft.metrics.peakNps <= source.profile.maxPeakNps + 1e-7);
    }
}

void LocalGenerationTest::repeatsKeepThemesAndSectionBoundariesSafe() {
    const auto source = request(repeated, 48);
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error), qPrintable(error));
    verifyDraft(source, analysis, draft); // Includes both hands across all segment boundaries.
    int compared = 0;
    for (const auto &segment : analysis.segments) {
        if (segment.repeatReference < 0) continue;
        const auto &reference = analysis.segments[segment.repeatReference];
        const auto comparison = lmsc::BeatmapPlayabilityValidator::compareMotifs(
            sectionNotes(draft, reference), reference.startBeat, sectionNotes(draft, segment), segment.startBeat);
        QVERIFY(comparison.comparable);
        QVERIFY2(comparison.actionDifference <= 0.2 + 1e-7, "重复音乐段落失去了主要左右手/切向主题");
        ++compared;
    }
    QVERIFY(compared >= 2);
}

void LocalGenerationTest::variableTempoAndOffsetPreserveSource() {
    auto source = request(variable, 42);
    source.timeMap = variableMap;
    source.allowedTypes = lmsc::DirectionalType | lmsc::DotType;
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error), qPrintable(error));
    verifyDraft(source, analysis, draft);
    QCOMPARE(draft.source.timeMap.changes().size(), 2);
    bool beforeChange = false, afterChange = false;
    for (const auto &object : draft.objects) {
        QVERIFY(std::abs(draft.source.timeMap.beatToSeconds(object.beat) - source.timeMap.beatToSeconds(object.beat)) < 1e-8);
        beforeChange |= object.beat < 24;
        afterChange |= object.beat >= 48;
    }
    QVERIFY(beforeChange);
    QVERIFY(afterChange);
}

void LocalGenerationTest::analysisOnlyAndInvalidRequests() {
    auto source = request();
    source.analysisOnly = true;
    source.allowedTypes = {};
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error), qPrintable(error));
    QVERIFY(draft.objects.isEmpty());
    QVERIFY(!draft.summary.isEmpty());
    QVERIFY(draft.source.analysisOnly);
    source.analysisOnly = false;
    QVERIFY(!lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error));
    QVERIFY(!error.isEmpty());
    source.allowedTypes = lmsc::GeneratedTypes(16);
    QVERIFY(!lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error));
    source.allowedTypes = lmsc::DirectionalType;
    source.profile.name = "Unknown";
    QVERIFY(!lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error));
}

void LocalGenerationTest::coreCancellationDoesNotPublishPartialDraft() {
    const auto source = request(repeated, 48);
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    bool cancel = false;
    QVERIFY(!lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error,
        [&] { return cancel; }, [&](int) { cancel = true; }));
    QVERIFY(draft.objects.isEmpty());
    QVERIFY(!error.isEmpty());
}

void LocalGenerationTest::serviceRunsOfflineAndKeepsGuiResponsive() {
    ProxyGuard proxy;
    lmsc::LocalAiGenerationService service;
    QVERIFY(service.isAvailable());
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(&service, &lmsc::AiGenerationService::requestFailed);
    auto source = request(longMusic, 180);
    service.generate(source);
    QCOMPARE(service.status().state, lmsc::AiGenerationService::Status::Running);
    QCOMPARE(ready.count(), 0); // generate returns before computation/completion is delivered.
    bool eventDelivered = false;
    QTimer::singleShot(0, &service, [&] { eventDelivered = true; });
    QTRY_VERIFY(eventDelivered);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() || failed.count(), 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QCOMPARE(ready.count(), 1);
    const auto draft = qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QVERIFY(!draft.objects.isEmpty());
    QCOMPARE(service.status().state, lmsc::AiGenerationService::Status::Completed);
    QCOMPARE(service.status().percent, 100);
    QVERIFY(service.isAvailable());
    service.discard(source.jobId);
    QCOMPARE(service.status().state, lmsc::AiGenerationService::Status::Idle);
}

void LocalGenerationTest::cacheMatchesSourceAndTiming() {
    lmsc::LocalAiGenerationService service;
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(&service, &lmsc::AiGenerationService::requestFailed);
    auto source = request();
    source.analysisOnly = true;
    service.generate(source);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() || failed.count(), 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QVERIFY(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).objects.isEmpty());
    QVERIFY(!service.property("localAnalysisCacheHit").toBool());
    source.analysisOnly = false;
    source.jobId = "use-cached-analysis";
    service.generate(source);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 2 || failed.count(), 30000);
    QVERIFY(failed.isEmpty());
    QVERIFY(service.property("localAnalysisCacheHit").toBool());
    QVERIFY(!qvariant_cast<lmsc::GenerationDraft>(ready.last().first()).objects.isEmpty());
    source.jobId = "changed-timing";
    source.analysisOnly = true;
    QVERIFY(source.timeMap.configure(120, 0.5));
    service.generate(source);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 3 || failed.count(), 30000);
    QVERIFY(failed.isEmpty());
    QVERIFY(!service.property("localAnalysisCacheHit").toBool());
    source.jobId = "changed-audio-revision";
    ++source.audioRevision;
    ++source.audio.revision;
    service.generate(source);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 4 || failed.count(), 30000);
    QVERIFY(failed.isEmpty());
    QVERIFY(!service.property("localAnalysisCacheHit").toBool());
    source.jobId = "changed-subdivision";
    source.profile = lmsc::DifficultyProfile::forName("Easy");
    service.generate(source);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 5 || failed.count(), 30000);
    QVERIFY(failed.isEmpty());
    QVERIFY(!service.property("localAnalysisCacheHit").toBool());
}

void LocalGenerationTest::cachedAnalysisRequiresExistingAudio() {
    const QString copiedPcm = temporary.filePath("cache-source.pcm");
    QVERIFY(QFile::copy(music, copiedPcm));
    lmsc::LocalAiGenerationService service;
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(&service, &lmsc::AiGenerationService::requestFailed);
    auto source = request(copiedPcm);
    source.analysisOnly = true;
    service.generate(source);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() || failed.count(), 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QCOMPARE(ready.count(), 1);
    QVERIFY(QFile::remove(copiedPcm));
    source.analysisOnly = false;
    source.jobId = "missing-cached-audio";
    service.generate(source);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 1);
    QVERIFY(!failed.first().at(1).toString().isEmpty());
    QCOMPARE(service.status().state, lmsc::AiGenerationService::Status::Failed);
}

void LocalGenerationTest::serviceRejectsMismatchedAudioRevision() {
    lmsc::LocalAiGenerationService service;
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(&service, &lmsc::AiGenerationService::requestFailed);
    auto source = request();
    ++source.audioRevision;
    service.generate(source);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.first().first().toString(), source.jobId);
    QCOMPARE(ready.count(), 0);
    QVERIFY(!failed.first().at(1).toString().isEmpty());
}

void LocalGenerationTest::replacementAndCancellationSuppressOldResults_data() {
    QTest::addColumn<bool>("reuseJobId");
    QTest::newRow("different-job") << false;
    QTest::newRow("same-job-new-revision") << true;
}

void LocalGenerationTest::replacementAndCancellationSuppressOldResults() {
    QFETCH(bool, reuseJobId);
    lmsc::LocalAiGenerationService service;
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(&service, &lmsc::AiGenerationService::requestFailed);
    QSignalSpy cancelled(&service, &lmsc::AiGenerationService::cancelled);
    auto old = request(longMusic, 180);
    service.generate(old);
    auto next = request();
    if (!reuseJobId) next.jobId = "replacement-job";
    next.documentRevision = old.documentRevision + 1;
    service.generate(next);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() || failed.count(), 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QCOMPARE(ready.count(), 1);
    auto draft = qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QCOMPARE(draft.source.jobId, next.jobId);
    QCOMPARE(draft.source.documentRevision, next.documentRevision);
    QTest::qWait(50);
    QCOMPARE(ready.count(), 1);
    old.jobId = "explicit-cancel";
    service.generate(old);
    service.cancel("unrelated-job");
    const int cancellationCount = cancelled.count();
    service.cancel(old.jobId);
    QCOMPARE(cancelled.count(), cancellationCount + 1);
    QCOMPARE(service.status().state, lmsc::AiGenerationService::Status::Idle);
    QTest::qWait(50);
    QCOMPARE(ready.count(), 1);
    QCOMPARE(failed.count(), 0);
}

void LocalGenerationTest::completedCancelledAndDestroyedWorkersReleaseAudioLease() {
    lmsc::LocalAiGenerationService service;
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    auto source = request();
    auto lease = std::make_shared<int>(1);
    std::weak_ptr<int> weak = lease;
    source.audio.lease = lease;
    lease.reset();
    service.generate(source);
    source.audio.lease.reset();
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30000);
    QVERIFY(!weak.expired()); // Preview owns the decoded audio until it is closed.
    ready.clear();
    QTRY_VERIFY(weak.expired()); // Analysis cache and Completed status keep no audio lease.
    source = request(longMusic, 180);
    lease = std::make_shared<int>(2);
    weak = lease;
    source.audio.lease = lease;
    lease.reset();
    service.generate(source);
    source.audio.lease.reset();
    service.cancel(source.jobId);
    QTRY_VERIFY(weak.expired());
    QCOMPARE(ready.count(), 0);
    auto *pending = new lmsc::LocalAiGenerationService;
    lease = std::make_shared<int>(3);
    weak = lease;
    source.audio.lease = lease;
    lease.reset();
    pending->generate(source);
    source.audio.lease.reset();
    delete pending;
    QTRY_VERIFY(weak.expired());
}

void LocalGenerationTest::progressReceiverMayDestroyService_data() {
    QTest::addColumn<bool>("onCompletion");
    QTest::newRow("initial-progress") << false;
    QTest::newRow("completion-progress") << true;
}

void LocalGenerationTest::progressReceiverMayDestroyService() {
    QFETCH(bool, onCompletion);
    auto *service = new lmsc::LocalAiGenerationService;
    QPointer<lmsc::LocalAiGenerationService> guard(service);
    QSignalSpy ready(service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(service, &lmsc::AiGenerationService::requestFailed);
    auto source = request();
    auto lease = std::make_shared<int>(4);
    std::weak_ptr<int> weak = lease;
    source.audio.lease = lease;
    lease.reset();
    bool deletionTriggered = false;
    QObject::connect(service, &lmsc::AiGenerationService::progress, this,
        [service, onCompletion, &deletionTriggered](const QString &, int percent, const QString &) {
            if ((onCompletion && percent == 100) || (!onCompletion && percent == 0)) {
                deletionTriggered = true;
                delete service;
            }
        }, Qt::DirectConnection);
    service->generate(source);
    source.audio.lease.reset();
    QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 30000);
    QVERIFY(deletionTriggered);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(failed.count(), 0);
    QTRY_VERIFY(weak.expired());
}

void LocalGenerationTest::cancellationReceiverMayGenerateReplacement() {
    lmsc::LocalAiGenerationService service;
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(&service, &lmsc::AiGenerationService::requestFailed);
    auto first = request(longMusic, 180);
    first.jobId = "cancelled-a";
    auto outer = request();
    outer.jobId = "outer-b";
    auto reentrant = request();
    reentrant.jobId = "reentrant-c";
    reentrant.documentRevision = 99;
    int replacements = 0;
    QObject::connect(&service, &lmsc::AiGenerationService::cancelled, this,
        [&](const QString &job) {
            if (job == first.jobId) {
                ++replacements;
                service.generate(reentrant);
            }
        }, Qt::DirectConnection);
    service.generate(first);
    service.generate(outer);
    QCOMPARE(replacements, 1);
    QCOMPARE(service.status().jobId, reentrant.jobId);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() || failed.count(), 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QCOMPARE(ready.count(), 1);
    const auto draft = qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QCOMPARE(draft.source.jobId, reentrant.jobId);
    QCOMPARE(draft.source.documentRevision, reentrant.documentRevision);
    QTest::qWait(30);
    QCOMPARE(ready.count(), 1);
    QCOMPARE(service.status().jobId, reentrant.jobId);
}

void LocalGenerationTest::cancellationReceiverMayDestroyService() {
    auto *service = new lmsc::LocalAiGenerationService;
    QPointer<lmsc::LocalAiGenerationService> guard(service);
    QSignalSpy ready(service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(service, &lmsc::AiGenerationService::requestFailed);
    auto first = request(longMusic, 180);
    first.jobId = "cancel-and-destroy";
    auto lease = std::make_shared<int>(5);
    std::weak_ptr<int> weak = lease;
    first.audio.lease = lease;
    lease.reset();
    service->generate(first);
    first.audio.lease.reset();
    bool deletionTriggered = false;
    QObject::connect(service, &lmsc::AiGenerationService::cancelled, this,
        [service, &deletionTriggered](const QString &) {
            deletionTriggered = true;
            delete service;
        }, Qt::DirectConnection);
    auto next = request();
    next.jobId = "outer-after-delete";
    service->generate(next);
    QVERIFY(guard.isNull());
    QVERIFY(deletionTriggered);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(failed.count(), 0);
    QTRY_VERIFY(weak.expired());
}

void LocalGenerationTest::threeMinuteBenchmark() {
    const QString benchmarkPcm = temporary.filePath("three-minutes-44100.pcm");
    lmsc::TimeMap map;
    QVERIFY(map.configure(120, 0));
    QVERIFY(writeMusic(benchmarkPcm, 180, map, false, 1.0, 44100));
    auto source = request(benchmarkPcm, 180);
    source.audio.sampleRate = 44100;
    source.allowedTypes = lmsc::DirectionalType | lmsc::DotType | lmsc::BombType | lmsc::WallType;
    lmsc::MusicAnalysis analysis;
    lmsc::GenerationDraft draft;
    QString error;
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source, &analysis, &error), qPrintable(error));
    const qint64 analysisMs = timer.elapsed();
    timer.restart();
    QVERIFY2(lmsc::LocalChartGenerator::generate(source, analysis, &draft, &error), qPrintable(error));
    const qint64 generationMs = timer.elapsed();
    qInfo().nospace() << "本地制谱实测：180 秒 / " << source.audio.sampleRate << " Hz / " << channels
        << " 声道；分析 " << analysisMs << " ms；编排 " << generationMs << " ms；击打 "
        << draft.metrics.directional + draft.metrics.dots << "；炸弹 " << draft.metrics.bombs
        << "；墙 " << draft.metrics.walls << "；平均 " << draft.metrics.averageNps
        << " NPS；两秒峰值 " << draft.metrics.peakNps << " NPS";
    QVERIFY(!draft.objects.isEmpty());
    verifyDraft(source, analysis, draft);
    bool lateHit = false;
    for (const auto &object : draft.objects)
        lateHit |= object.kind == lmsc::ObjectKind::Note && source.timeMap.beatToSeconds(object.beat) > 170;
    QVERIFY(lateHit);
}

QTEST_GUILESS_MAIN(LocalGenerationTest)
#include "LocalGenerationTest.moc"
