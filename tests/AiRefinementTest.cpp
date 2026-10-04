#include "core/AiRefinementService.h"
#include "core/AiTextTransport.h"
#include "core/LocalChartGenerator.h"
#include "core/MusicFeatureAnalyzer.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <functional>

namespace {
bool fixture(const QString &path, lmsc::RefinementRequest *request, double duration = 32, double pulseSpacing = .25) {
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray bytes;
    for (int i = 0; i < int(duration * 2000); ++i) {
        const double time = i / 2000.0, phase = std::fmod(time, pulseSpacing);
        const double pulse = phase < .06 ? .75 * std::exp(-phase * 45) : 0;
        const qint16 value = qint16((.025 + pulse) * std::sin(2 * 3.141592653589793 * 110 * time) * 30000);
        char sample[2]; qToLittleEndian<qint16>(value, reinterpret_cast<uchar *>(sample)); bytes.append(sample, 2); bytes.append(sample, 2);
    }
    if (file.write(bytes) != bytes.size()) return false; file.close();
    auto &generation = request->generation;
    generation.jobId = "refine"; generation.documentId = "document"; generation.documentRevision = 3;
    generation.audioRevision = 7; generation.audio = {path, {}, 7, 2000, 2, duration, {}};
    generation.profile = lmsc::DifficultyProfile::forName("Hard"); generation.allowedTypes = lmsc::DirectionalType | lmsc::DotType;
    generation.timeMap.configure(120, 0);
    lmsc::MusicAnalysis analysis; lmsc::GenerationDraft draft; QString error;
    if (!lmsc::MusicFeatureAnalyzer::analyze(generation, &analysis, &error)
        || !lmsc::LocalChartGenerator::generate(generation, analysis, &draft, &error)) return false;
    request->baseline = draft.objects;
    for (int i = 0; i < request->baseline.size(); ++i) request->baseline[i].id = QStringLiteral("original-%1").arg(i);
    if (!request->baseline.isEmpty()) request->baseline.first().preservedCustomData = {{"_color", QJsonArray{.2,.5,.8,1}}};
    request->baselineHash = lmsc::refinementBaselineHash(request->baseline); return !request->baseline.isEmpty();
}
bool selectPhrase(lmsc::RefinementRequest *request, int index = 0) {
    lmsc::MusicAnalysis analysis; QString error;
    if (!lmsc::MusicFeatureAnalyzer::analyze(request->generation, &analysis, &error)) return false;
    if (analysis.phrases.isEmpty()) analysis.rebuildPhrases(request->generation.timeMap);
    if (index < 0 || index >= analysis.phrases.size()) return false;
    request->selectedOnly = true; request->startSeconds = analysis.phrases[index].startSeconds;
    request->endSeconds = analysis.phrases[index].endSeconds; return true;
}
QJsonObject emptyPatch(const QJsonObject &input) {
    return {{"schemaVersion", 1}, {"segmentId", input.value("segmentId")}, {"baselineHash", input.value("baselineHash")},
        {"updates", QJsonArray{}}, {"removals", QJsonArray{}}, {"additions", QJsonArray{}}};
}
QJsonObject update(const QJsonObject &note, int direction) {
    return {{"id", note.value("id")}, {"anchorId", ""}, {"x", note.value("x")}, {"y", note.value("y")},
        {"hand", note.value("hand")}, {"direction", direction}};
}
class Transport : public lmsc::AiTextTransport {
public:
    bool hold = false, available = true;
    int failAt = -1;
    QVector<lmsc::AiTextRequest> requests;
    QStringList cancelled;
    std::function<QJsonObject(const QJsonObject &, int)> responder;
    bool isAvailable() const override { return available; }
    void configure(const lmsc::AppPreferences &) override {}
    void complete(const lmsc::AiTextRequest &message) override {
        requests.append(message); if (hold) return;
        const int call = requests.size();
        const auto input = QJsonDocument::fromJson(message.userPrompt.toUtf8()).object();
        const auto response = responder ? responder(input, call) : emptyPatch(input);
        const QString text = QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
        QTimer::singleShot(0, this, [this, message, text, call] {
            if (call == failAt) emit failed(message.requestId, QStringLiteral("模拟断网"));
            else emit completed({message.requestId, text, 1, 1});
        });
    }
    void cancel(const QString &id) override { cancelled.append(id); }
};
}
class AiRefinementTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<lmsc::RefinementResult>(); }
    void hashIncludesCustomAndProtection();
    void metadataUpdatePreservesBaselineAndCustom();
    void rhythmBudgetIsSharedAcrossDeleteAddAndMove();
    void realHitAndOneBeatMovementAreRequired_data();
    void realHitAndOneBeatMovementAreRequired();
    void readOnlyContextCannotBeEdited();
    void quickSameHandCutsAreRejected();
    void selectedRangeStaysUnchangedOutside();
    void batchHasFiveTargetsAndCanContinue();
    void networkPausePreservesSuccessAndCanContinue();
    void cancelPreservesBaselineAndRejectsLateResult();
    void staleBaselineAndMissingIdsFail();
    void connectionChangePausesWithoutLosingCandidate();
    void candidateSignalCanDeleteService();
    void progressCanReplaceRequest();
    void resumedRepairKeepsOneRepairBudget();
    void changedAudioStopsResumingAndKeepsCandidate();
    void progressCanDeleteService();
    void legalDeleteAndAddUseRealHit();
    void legalSmallMoveUsesFreeHit();
    void shortPhrasesAreIndependentTargetsAndContinue();
    void phraseBudgetCannotBorrowFromContainingBlock();
};
void AiRefinementTest::hashIncludesCustomAndProtection() {
    lmsc::BeatObject note; note.id = "a"; const QVector<lmsc::BeatObject> before{note};
    auto custom = before; custom[0].preservedCustomData = {{"future", true}};
    QVERIFY(lmsc::refinementBaselineHash(before) != lmsc::refinementBaselineHash(custom));
    auto protectedNote = before; protectedNote[0].protectedReason = "protected";
    QVERIFY(lmsc::refinementBaselineHash(before) != lmsc::refinementBaselineHash(protectedNote));
    const auto patch = lmsc::refinementDifference(before, custom); QCOMPARE(patch.updates.size(), 1);
    QCOMPARE(lmsc::refinementStatistics(patch, before).changed, 1);
}
void AiRefinementTest::metadataUpdatePreservesBaselineAndCustom() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    Transport transport;
    transport.responder = [](const QJsonObject &input, int) {
        auto patch = emptyPatch(input);
        for (const auto &value : input.value("notes").toArray()) {
            const auto note = value.toObject(); if (note.value("direction").toInt() != 8) { patch.insert("updates", QJsonArray{update(note, 8)}); break; }
        }
        return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1);
    const auto result = qvariant_cast<lmsc::RefinementResult>(ready.first().first());
    QVERIFY(result.stats.changed > 0); QCOMPARE(result.stats.moved, 0); QCOMPARE(result.stats.added, 0); QCOMPARE(result.stats.removed, 0);
    QCOMPARE(lmsc::refinementBaselineHash(result.source.baseline), request.baselineHash);
    for (const auto &note : result.candidate.objects) for (const auto &prior : request.baseline) if (note.id == prior.id) QCOMPARE(note.preservedCustomData, prior.preservedCustomData);
    QVERIFY(!transport.requests.first().userPrompt.contains(request.generation.audio.path));
    QVERIFY(transport.requests.first().userPrompt.contains("readOnlyContext"));
    QVERIFY(!transport.requests.first().userPrompt.contains("original-"));
}
void AiRefinementTest::rhythmBudgetIsSharedAcrossDeleteAddAndMove() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    QVERIFY(selectPhrase(&request));
    Transport transport;
    transport.responder = [](const QJsonObject &input, int) {
        auto patch = emptyPatch(input); QJsonArray removals;
        const auto notes = input.value("notes").toArray(); const int budget = input.value("rhythmChangeBudget").toInt();
        for (int i = 0; i < qMin(notes.size(), budget); ++i) removals.append(notes[i].toObject().value("id"));
        patch.insert("removals", removals);
        const auto note = notes[qMin(budget, notes.size() - 1)].toObject();
        auto changed = update(note, note.value("direction").toInt());
        for (const auto &item : input.value("hitAnchors").toArray()) {
            const auto anchor = item.toObject();
            const double movement = anchor.value("beat").toDouble() - note.value("beat").toDouble();
            if (movement > 1e-7 && movement <= 1.0 + 1e-7) { changed.insert("anchorId", anchor.value("id")); break; }
        }
        patch.insert("updates", QJsonArray{changed});
        const auto anchor = input.value("hitAnchors").toArray().first().toObject();
        patch.insert("additions", QJsonArray{QJsonObject{{"anchorId", anchor.value("id")}, {"x", 1}, {"y", 1}, {"hand", "left"}, {"direction", 8}}});
        return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QCOMPARE(transport.requests.size(), 2);
    const auto result = qvariant_cast<lmsc::RefinementResult>(ready.first().first());
    QVERIFY(result.patch.isEmpty()); QVERIFY(!result.candidate.warnings.isEmpty());
    QVERIFY(transport.requests.last().userPrompt.contains(QStringLiteral("节奏改动额度")));
}
void AiRefinementTest::realHitAndOneBeatMovementAreRequired_data() {
    QTest::addColumn<bool>("fabricated"); QTest::newRow("fabricated-onset") << true; QTest::newRow("move-over-one-beat") << false;
}
void AiRefinementTest::realHitAndOneBeatMovementAreRequired() {
    QFETCH(bool, fabricated);
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    QVERIFY(selectPhrase(&request));
    Transport transport;
    transport.responder = [fabricated](const QJsonObject &input, int) {
        auto patch = emptyPatch(input); const auto note = input.value("notes").toArray().first().toObject();
        auto row = update(note, note.value("direction").toInt()); QString anchor = "fabricated";
        if (!fabricated) for (const auto &item : input.value("hitAnchors").toArray()) if (item.toObject().value("beat").toDouble() > note.value("beat").toDouble() + 1.0) { anchor = item.toObject().value("id").toString(); break; }
        row.insert("anchorId", anchor); patch.insert("updates", QJsonArray{row}); return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QCOMPARE(transport.requests.size(), 2);
    QVERIFY(qvariant_cast<lmsc::RefinementResult>(ready.first().first()).patch.isEmpty());
}
void AiRefinementTest::readOnlyContextCannotBeEdited() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    QVERIFY(selectPhrase(&request, 4));
    Transport transport;
    transport.responder = [](const QJsonObject &input, int) {
        auto patch = emptyPatch(input); const auto rows = input.value("readOnlyContext").toArray();
        if (!rows.isEmpty()) patch.insert("updates", QJsonArray{update(rows.first().toObject(), 8)}); return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QCOMPARE(transport.requests.size(), 2);
    QVERIFY(qvariant_cast<lmsc::RefinementResult>(ready.first().first()).patch.isEmpty());
}
void AiRefinementTest::quickSameHandCutsAreRejected() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    QVERIFY(selectPhrase(&request));
    Transport transport;
    transport.responder = [](const QJsonObject &input, int) {
        auto patch = emptyPatch(input); QJsonArray updates;
        for (const auto &item : input.value("notes").toArray()) {
            const auto note = item.toObject(); if (note.value("hand").toString() != "left") continue;
            updates.append(update(note, 1)); if (updates.size() == 2) break;
        }
        patch.insert("updates", updates); return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QCOMPARE(transport.requests.size(), 2);
    QVERIFY(qvariant_cast<lmsc::RefinementResult>(ready.first().first()).patch.isEmpty());
    QVERIFY(transport.requests.last().userPrompt.contains(QStringLiteral("回刀")));
}
void AiRefinementTest::selectedRangeStaysUnchangedOutside() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    request.selectedOnly = true; request.startSeconds = 9; request.endSeconds = 9.75;
    Transport transport;
    transport.responder = [](const QJsonObject &input, int) {
        auto patch = emptyPatch(input);
        for (const auto &item : input.value("notes").toArray()) {
            const auto note = item.toObject(); if (note.value("direction").toInt() != 8) { patch.insert("updates", QJsonArray{update(note, 8)}); break; }
        }
        return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1);
    const auto result = qvariant_cast<lmsc::RefinementResult>(ready.first().first());
    QCOMPARE(result.processedSegments.size(), 1); QCOMPARE(transport.requests.size(), 1);
    for (const auto &note : request.baseline) if (request.generation.timeMap.beatToSeconds(note.beat) < 9 || request.generation.timeMap.beatToSeconds(note.beat) >= 9.75) {
        bool found = false; for (const auto &after : result.candidate.objects) if (after.id == note.id) { found = true; QCOMPARE(lmsc::refinementBaselineHash({after}), lmsc::refinementBaselineHash({note})); } QVERIFY(found);
    }
    const auto sent = QJsonDocument::fromJson(transport.requests.first().userPrompt.toUtf8()).object(); QCOMPARE(sent.value("startSeconds").toDouble(), 9.0); QCOMPARE(sent.value("endSeconds").toDouble(), 9.75);
}
void AiRefinementTest::batchHasFiveTargetsAndCanContinue() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request, 96));
    Transport transport; lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QCOMPARE(transport.requests.size(), 5);
    auto result = qvariant_cast<lmsc::RefinementResult>(ready.first().first()); QVERIFY(result.resumable); QCOMPARE(result.processedSegments.size(), 5);
    service.resume("refine"); QTRY_COMPARE(ready.count(), 2); QCOMPARE(transport.requests.size(), 10);
    result = qvariant_cast<lmsc::RefinementResult>(ready.last().first()); QCOMPARE(result.processedSegments.size(), 10); QVERIFY(result.patch.isEmpty());
}
void AiRefinementTest::networkPausePreservesSuccessAndCanContinue() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    Transport transport; transport.failAt = 2;
    transport.responder = [](const QJsonObject &input, int call) {
        auto patch = emptyPatch(input); if (call == 1) for (const auto &item : input.value("notes").toArray()) {
            const auto note = item.toObject(); if (note.value("direction").toInt() != 8) { patch.insert("updates", QJsonArray{update(note, 8)}); break; }
        } return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady), failed(&service, &lmsc::AiRefinementService::requestFailed);
    service.refine(request); QTRY_COMPARE(failed.count(), 1); QCOMPARE(ready.count(), 1);
    auto result = qvariant_cast<lmsc::RefinementResult>(ready.first().first()); QVERIFY(result.stats.changed > 0); QVERIFY(result.resumable);
    transport.failAt = -1; service.resume("refine"); QTRY_COMPARE(ready.count(), 2);
    result = qvariant_cast<lmsc::RefinementResult>(ready.last().first()); QVERIFY(result.stats.changed > 0); QCOMPARE(result.source.baselineHash, request.baselineHash);
}
void AiRefinementTest::cancelPreservesBaselineAndRejectsLateResult() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    Transport transport; transport.hold = true; lmsc::AiRefinementService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady), cancelled(&service, &lmsc::AiRefinementService::cancelled);
    service.refine(request); QTRY_COMPARE(transport.requests.size(), 1); const auto old = transport.requests.first(); service.cancel("refine");
    QCOMPARE(ready.count(), 1); QCOMPARE(cancelled.count(), 1);
    emit transport.completed({old.requestId, "{}", 1, 1}); QTest::qWait(20); QCOMPARE(ready.count(), 1);
    QVERIFY(qvariant_cast<lmsc::RefinementResult>(ready.first().first()).patch.isEmpty());
}
void AiRefinementTest::staleBaselineAndMissingIdsFail() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    Transport transport; lmsc::AiRefinementService service(&transport); QSignalSpy failed(&service, &lmsc::AiRefinementService::requestFailed);
    auto changed = request; changed.baseline.first().direction = 8; service.refine(changed); QCOMPARE(failed.count(), 1);
    changed = request; changed.baseline.first().id.clear(); changed.baselineHash = lmsc::refinementBaselineHash(changed.baseline);
    service.refine(changed); QCOMPARE(failed.count(), 2); QCOMPARE(transport.requests.size(), 0);
}
void AiRefinementTest::connectionChangePausesWithoutLosingCandidate() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    Transport transport; transport.hold = true; lmsc::AiRefinementService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady), failed(&service, &lmsc::AiRefinementService::requestFailed);
    service.refine(request); QTRY_COMPARE(transport.requests.size(), 1); emit transport.configurationChanged();
    QCOMPARE(ready.count(), 1); QCOMPARE(failed.count(), 1); QVERIFY(service.status().resumable); QCOMPARE(transport.cancelled.size(), 1);
}
void AiRefinementTest::candidateSignalCanDeleteService() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    Transport transport; QPointer<lmsc::AiRefinementService> service = new lmsc::AiRefinementService(&transport);
    connect(service, &lmsc::AiRefinementService::candidateReady, this, [&service] { delete service.data(); });
    service->refine(request); QTRY_VERIFY(service.isNull());
}
void AiRefinementTest::progressCanReplaceRequest() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    Transport transport; lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    bool replaced = false;
    connect(&service, &lmsc::AiRefinementService::progress, this, [&](const QString &job, int, const QString &stage) {
        if (!replaced && job == "refine" && stage.contains(QStringLiteral("AI 精修"))) { replaced = true; auto newer = request; newer.generation.jobId = "newer"; service.refine(newer); }
    });
    service.refine(request); QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(qvariant_cast<lmsc::RefinementResult>(ready.first().first()).source.generation.jobId, QString("newer"));
}
void AiRefinementTest::resumedRepairKeepsOneRepairBudget() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    QVERIFY(selectPhrase(&request));
    Transport transport; transport.failAt = 2;
    transport.responder = [](const QJsonObject &, int) { return QJsonObject{}; };
    lmsc::AiRefinementService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady), failed(&service, &lmsc::AiRefinementService::requestFailed);
    service.refine(request); QTRY_COMPARE(failed.count(), 1); QCOMPARE(transport.requests.size(), 2);
    transport.failAt = -1; service.resume("refine"); QTRY_COMPARE(ready.count(), 2);
    QCOMPARE(transport.requests.size(), 3); QVERIFY(!service.status().resumable);
    QVERIFY(!qvariant_cast<lmsc::RefinementResult>(ready.last().first()).candidate.warnings.isEmpty());
}
void AiRefinementTest::changedAudioStopsResumingAndKeepsCandidate() {
    QTemporaryDir dir; lmsc::RefinementRequest request; const QString path = dir.filePath("song.pcm"); QVERIFY(fixture(path, &request, 96));
    Transport transport; lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady), failed(&service, &lmsc::AiRefinementService::requestFailed);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QVERIFY(service.status().resumable);
    QFile audio(path); QVERIFY(audio.open(QIODevice::Append)); QCOMPARE(audio.write("\0\0\0\0", 4), qint64(4)); audio.close();
    service.resume("refine"); QCOMPARE(failed.count(), 1); QCOMPARE(transport.requests.size(), 5); QVERIFY(!service.status().resumable);
    QCOMPARE(ready.count(), 2); QVERIFY(!qvariant_cast<lmsc::RefinementResult>(ready.last().first()).resumable);
}
void AiRefinementTest::progressCanDeleteService() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    Transport transport; QPointer<lmsc::AiRefinementService> service = new lmsc::AiRefinementService(&transport);
    connect(service, &lmsc::AiRefinementService::progress, this, [&service](const QString &, int, const QString &stage) {
        if (stage.contains(QStringLiteral("AI 精修"))) delete service.data();
    });
    service->refine(request); QTRY_VERIFY(service.isNull()); QCOMPARE(transport.requests.size(), 0);
}
void AiRefinementTest::legalDeleteAndAddUseRealHit() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request, 32, .5));
    // A sparse eight-beat phrase permits two rhythm operations when both
    // hands strike its real quarter-note onsets; four-beat Hard phrases do not.
    request.generation.profile = lmsc::DifficultyProfile::forName("Expert");
    lmsc::MusicAnalysis analysis; QString error;
    QVERIFY(lmsc::MusicFeatureAnalyzer::analyze(request.generation, &analysis, &error));
    request.baseline.clear();
    for (const auto &anchor : analysis.anchors) if (anchor.kind == lmsc::MusicAnchorKind::Hit)
        for (int hand = 0; hand < 2; ++hand) {
            lmsc::BeatObject note; note.id = QStringLiteral("pair-%1-%2").arg(anchor.id).arg(hand);
            note.beat = anchor.beat; note.color = hand; note.x = hand == 0 ? 1 : 2; note.y = 1; note.direction = 8;
            request.baseline.append(note);
        }
    request.baselineHash = lmsc::refinementBaselineHash(request.baseline);
    QVERIFY(selectPhrase(&request));
    Transport transport; bool supplied = false;
    transport.responder = [&supplied](const QJsonObject &input, int) {
        auto patch = emptyPatch(input); const auto note = input.value("notes").toArray().first().toObject();
        if (input.value("rhythmChangeBudget").toInt() < 2) return patch;
        for (const auto &item : input.value("hitAnchors").toArray()) {
            const auto anchor = item.toObject();
            if (std::abs(anchor.value("beat").toDouble() - note.value("beat").toDouble()) > 1e-7) continue;
            patch.insert("removals", QJsonArray{note.value("id")});
            patch.insert("additions", QJsonArray{QJsonObject{{"anchorId", anchor.value("id")}, {"x", note.value("x")},
                {"y", note.value("y")}, {"hand", note.value("hand")}, {"direction", note.value("direction")}}});
            supplied = true; break;
        }
        return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QVERIFY(supplied); QCOMPARE(transport.requests.size(), 1);
    const auto result = qvariant_cast<lmsc::RefinementResult>(ready.first().first());
    QCOMPARE(result.stats.added, 1); QCOMPARE(result.stats.removed, 1); QCOMPARE(result.candidate.objects.size(), request.baseline.size());
    QVERIFY(!result.patch.additions.first().id.isEmpty()); QVERIFY(!result.patch.additions.first().id.startsWith("n"));
}
void AiRefinementTest::legalSmallMoveUsesFreeHit() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    for (auto &note : request.baseline) { note.direction = 8; note.x = note.color == 0 ? 1 : 2; note.y = 1; }
    request.baselineHash = lmsc::refinementBaselineHash(request.baseline);
    QVERIFY(selectPhrase(&request));
    Transport transport; bool supplied = false;
    transport.responder = [&supplied](const QJsonObject &input, int) {
        auto patch = emptyPatch(input); const auto notes = input.value("notes").toArray();
        QJsonArray surroundings = notes; for (const auto &item : input.value("readOnlyContext").toArray()) surroundings.append(item);
        for (const auto &item : notes) {
            const auto note = item.toObject();
            for (const auto &value : input.value("hitAnchors").toArray()) {
                const auto anchor = value.toObject(); const double beat = anchor.value("beat").toDouble();
                const double delta = std::abs(beat - note.value("beat").toDouble());
                if (delta < 1e-7 || delta > 1.0 + 1e-7) continue;
                bool safe = true;
                for (const auto &otherItem : surroundings) {
                    const auto other = otherItem.toObject();
                    if (other.value("id") == note.value("id") || other.value("kind").toInt() != 0 || other.value("hand") != note.value("hand")) continue;
                    if (std::abs(other.value("beat").toDouble() - beat) < .5 - 1e-7) safe = false;
                }
                if (!safe) continue;
                auto row = update(note, 8); row.insert("anchorId", anchor.value("id")); patch.insert("updates", QJsonArray{row}); supplied = true; return patch;
            }
        }
        return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QVERIFY(supplied); QCOMPARE(transport.requests.size(), 1);
    const auto result = qvariant_cast<lmsc::RefinementResult>(ready.first().first()); QCOMPARE(result.stats.moved, 1); QCOMPARE(result.stats.changed, 1);
    QCOMPARE(result.stats.added, 0); QCOMPARE(result.stats.removed, 0);
}
void AiRefinementTest::shortPhrasesAreIndependentTargetsAndContinue() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request, 12));
    lmsc::MusicAnalysis analysis; QString error;
    QVERIFY(lmsc::MusicFeatureAnalyzer::analyze(request.generation, &analysis, &error));
    QVERIFY(analysis.phrases.size() > analysis.segments.size()); QCOMPARE(analysis.phrases.size(), 6);
    Transport transport; lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QCOMPARE(transport.requests.size(), 5);
    auto result = qvariant_cast<lmsc::RefinementResult>(ready.first().first());
    QCOMPARE(result.processedSegments.size(), 5); QCOMPARE(result.processedRanges.size(), 5);
    QCOMPARE(result.remainingSegments.size(), 1); QVERIFY(result.resumable);
    QSet<QString> phraseIds, blockIds;
    for (const auto &message : transport.requests) {
        const auto input = QJsonDocument::fromJson(message.userPrompt.toUtf8()).object();
        const auto music = input.value("music").toObject();
        const auto phraseId = input.value("segmentId").toString();
        QVERIFY(phraseId.startsWith("p")); QCOMPARE(phraseId, music.value("phraseId").toString());
        phraseIds.insert(phraseId); blockIds.insert(input.value("blockId").toString());
        const double span = music.value("endBeat").toDouble() - music.value("startBeat").toDouble();
        QVERIFY(std::abs(span - 4) < 1e-7 || std::abs(span - 8) < 1e-7);
        for (const auto &item : input.value("notes").toArray()) {
            const double seconds = request.generation.timeMap.beatToSeconds(item.toObject().value("beat").toDouble());
            QVERIFY(seconds >= input.value("startSeconds").toDouble() - 1e-7);
            QVERIFY(seconds < input.value("endSeconds").toDouble() - 1e-7);
        }
        for (const auto &item : input.value("readOnlyContext").toArray()) {
            const auto note = item.toObject(); QVERIFY(!note.value("editable").toBool());
            const double seconds = request.generation.timeMap.beatToSeconds(note.value("beat").toDouble());
            QVERIFY(seconds >= input.value("startSeconds").toDouble() - 2 - 1e-7);
            QVERIFY(seconds < input.value("endSeconds").toDouble() + 2 + 1e-7);
        }
    }
    QCOMPARE(phraseIds.size(), 5); QVERIFY(blockIds.size() < phraseIds.size());
    service.resume("refine"); QTRY_COMPARE(ready.count(), 2); QCOMPARE(transport.requests.size(), 6);
    result = qvariant_cast<lmsc::RefinementResult>(ready.last().first());
    QCOMPARE(result.processedSegments.size(), 6); QVERIFY(result.remainingSegments.isEmpty()); QVERIFY(!result.resumable);
    for (const auto &id : result.processedSegments) phraseIds.insert(id); QCOMPARE(phraseIds.size(), 6);
}
void AiRefinementTest::phraseBudgetCannotBorrowFromContainingBlock() {
    QTemporaryDir dir; lmsc::RefinementRequest request; QVERIFY(fixture(dir.filePath("song.pcm"), &request));
    QVERIFY(selectPhrase(&request));
    lmsc::MusicAnalysis analysis; QString error;
    QVERIFY(lmsc::MusicFeatureAnalyzer::analyze(request.generation, &analysis, &error));
    const auto &phrase = analysis.phrases.first(); const auto &block = analysis.segments[phrase.segmentIndex];
    int blockNotes = 0;
    for (const auto &note : request.baseline) if (note.beat >= block.startBeat - 1e-7 && note.beat < block.endBeat - 1e-7) ++blockNotes;
    Transport transport;
    transport.responder = [](const QJsonObject &input, int) {
        auto patch = emptyPatch(input); QJsonArray removals; const auto notes = input.value("notes").toArray();
        for (int i = 0; i < input.value("rhythmChangeBudget").toInt() + 1 && i < notes.size(); ++i) removals.append(notes[i].toObject().value("id"));
        patch.insert("removals", removals); return patch;
    };
    lmsc::AiRefinementService service(&transport); QSignalSpy ready(&service, &lmsc::AiRefinementService::candidateReady);
    service.refine(request); QTRY_COMPARE(ready.count(), 1); QCOMPARE(transport.requests.size(), 2);
    const auto input = QJsonDocument::fromJson(transport.requests.first().userPrompt.toUtf8()).object();
    const int phraseBudget = qMin(8, qMax(1, int(std::floor(input.value("notes").toArray().size() * .15))));
    QCOMPARE(input.value("rhythmChangeBudget").toInt(), phraseBudget);
    QVERIFY(qMin(8, qMax(1, int(std::floor(blockNotes * .15)))) > phraseBudget);
    QVERIFY(transport.requests.last().userPrompt.contains(QStringLiteral("节奏改动额度")));
    QVERIFY(qvariant_cast<lmsc::RefinementResult>(ready.first().first()).patch.isEmpty());
}
QTEST_GUILESS_MAIN(AiRefinementTest)
#include "AiRefinementTest.moc"
