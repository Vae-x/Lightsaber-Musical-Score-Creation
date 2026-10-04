#include "core/HybridAiGenerationService.h"
#include "core/AiTextTransport.h"
#include "core/BeatmapPlayabilityValidator.h"
#include "core/MusicFeatureAnalyzer.h"
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <functional>

namespace {
bool writePcm(const QString &path, double duration = 32) {
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray bytes;
    for (int i = 0; i < int(duration * 2000); ++i) {
        const double time = i / 2000.0, phase = std::fmod(time, .25);
        const double pulse = phase < .06 ? .75 * std::exp(-phase * 45) : 0;
        const qint16 value = qint16((.025 + pulse) * std::sin(2 * 3.141592653589793 * 110 * time) * 30000);
        char sample[2]; qToLittleEndian<qint16>(value, reinterpret_cast<uchar *>(sample)); bytes.append(sample, 2); bytes.append(sample, 2);
    }
    return file.write(bytes) == bytes.size();
}
lmsc::GenerationRequest request(const QString &path, const QString &job = "hybrid") {
    lmsc::GenerationRequest result;
    result.jobId = job; result.documentId = "document"; result.documentRevision = 3; result.audioRevision = 7;
    result.audio = {path, {}, 7, 2000, 2, 32, {}};
    result.profile = lmsc::DifficultyProfile::forName("Hard"); result.allowedTypes = lmsc::DirectionalType | lmsc::DotType;
    result.timeMap.configure(120, 0); return result;
}
class Transport : public lmsc::AiTextTransport {
public:
    bool available = true, hold = false, fail = false, invalid = false;
    QString identity = "mock-connection";
    QVector<lmsc::AiTextRequest> requests;
    QStringList cancellations;
    std::function<void()> onCancel;
    bool isAvailable() const override { return available; }
    void configure(const lmsc::AppPreferences &) override {}
    QString connectionIdentity() const override { return identity; }
    void complete(const lmsc::AiTextRequest &message) override {
        requests.append(message); if (hold) return;
        const auto input = QJsonDocument::fromJson(message.userPrompt.toUtf8()).object();
        const QString text = invalid ? "{}" : QString::fromUtf8(QJsonDocument(input.value("localProposal").toObject()).toJson(QJsonDocument::Compact));
        QTimer::singleShot(0, this, [this, message, text] {
            if (fail) emit failed(message.requestId, QStringLiteral("模拟连接失败"));
            else emit completed({message.requestId, text, 1, 1});
        });
    }
    void cancel(const QString &id) override { cancellations.append(id); if (onCancel) onCancel(); }
};
}
class HybridGenerationTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<lmsc::GenerationDraft>(); qRegisterMetaType<lmsc::SongArrangementPlan>(); }
    void onePlanningRequestAndLocalNotes();
    void offlineFallback();
    void failureFallback();
    void invalidPlanIsRepairedOnce();
    void analysisAndPlanCachesReuseDifferentSeed();
    void connectionChangeInvalidatesPlanOnly();
    void skipAndStaleResponse();
    void cancelStopsWholeFlow();
    void configurationDuringAnalysisDoesNotStartSecondWorker();
    void planSignalCanDeleteService();
    void planningProgressCanReplaceRequest();
    void planningProgressCanDeleteService();
    void cancelledTransportCanDeleteService();
};
void HybridGenerationTest::onePlanningRequestAndLocalNotes() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; lmsc::HybridAiGenerationService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady), plan(&service, &lmsc::HybridAiGenerationService::planReady);
    const auto input = request(path); service.generate(input); QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(transport.requests.size(), 1); QCOMPARE(plan.count(), 1);
    const auto draft = qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QVERIFY(!draft.objects.isEmpty()); QVERIFY(draft.arrangement); QCOMPARE(draft.arrangement->source, QString("ai"));
    lmsc::MusicAnalysis analysis; QString error; QVERIFY(lmsc::MusicFeatureAnalyzer::analyze(input, &analysis, &error));
    QStringList errors; QVERIFY2(lmsc::BeatmapPlayabilityValidator::validateObjects(draft.objects, input, analysis, &errors), qPrintable(errors.join('\n')));
    const auto sent = QJsonDocument::fromJson(transport.requests.first().userPrompt.toUtf8()).object();
    QCOMPARE(sent.value("stage").toString(), QString("arrangement")); QVERIFY(!sent.contains("notes"));
    QVERIFY(!transport.requests.first().userPrompt.contains(path));
}
void HybridGenerationTest::offlineFallback() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; transport.available = false; lmsc::HybridAiGenerationService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady); service.generate(request(path)); QTRY_COMPARE(ready.count(), 1);
    const auto draft = qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QCOMPARE(transport.requests.size(), 0); QCOMPARE(draft.arrangement->source, QString("local")); QVERIFY(!draft.warnings.isEmpty());
}
void HybridGenerationTest::failureFallback() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; transport.fail = true; lmsc::HybridAiGenerationService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady), failed(&service, &lmsc::AiGenerationService::requestFailed);
    service.generate(request(path)); QTRY_COMPARE(ready.count(), 1); QCOMPARE(failed.count(), 0); QCOMPARE(transport.requests.size(), 1);
    QVERIFY(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).warnings.join('\n').contains(QStringLiteral("失败")));
}
void HybridGenerationTest::invalidPlanIsRepairedOnce() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; transport.invalid = true; lmsc::HybridAiGenerationService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady); service.generate(request(path)); QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(transport.requests.size(), 2);
    QVERIFY(QJsonDocument::fromJson(transport.requests.last().userPrompt.toUtf8()).object().contains("validationErrors"));
    QCOMPARE(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).arrangement->source, QString("local"));
}
void HybridGenerationTest::analysisAndPlanCachesReuseDifferentSeed() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; lmsc::HybridAiGenerationService service(&transport); QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    auto input = request(path); service.generate(input); QTRY_COMPARE(ready.count(), 1);
    input.jobId = "hybrid-next"; input.arrangementSeed = 987; service.generate(input); QTRY_COMPARE(ready.count(), 2);
    QCOMPARE(transport.requests.size(), 1); QVERIFY(service.property("hybridAnalysisCacheHit").toBool()); QVERIFY(service.property("hybridPlanCacheHit").toBool());
    QCOMPARE(qvariant_cast<lmsc::GenerationDraft>(ready.last().first()).source.arrangementSeed, quint32(987));
}
void HybridGenerationTest::connectionChangeInvalidatesPlanOnly() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; lmsc::HybridAiGenerationService service(&transport); QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    service.generate(request(path)); QTRY_COMPARE(ready.count(), 1);
    transport.identity = "other"; emit transport.configurationChanged(); service.generate(request(path, "next")); QTRY_COMPARE(ready.count(), 2);
    QCOMPARE(transport.requests.size(), 2); QVERIFY(service.property("hybridAnalysisCacheHit").toBool()); QVERIFY(!service.property("hybridPlanCacheHit").toBool());
}
void HybridGenerationTest::skipAndStaleResponse() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; transport.hold = true; lmsc::HybridAiGenerationService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady); service.generate(request(path)); QTRY_COMPARE(transport.requests.size(), 1);
    const auto old = transport.requests.first(); service.skipPlanning("hybrid"); QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(transport.cancellations.size(), 1);
    emit transport.completed({old.requestId, "{}", 1, 1}); QTest::qWait(20); QCOMPARE(ready.count(), 1);
}
void HybridGenerationTest::cancelStopsWholeFlow() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; transport.hold = true; lmsc::HybridAiGenerationService service(&transport);
    QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady), cancelled(&service, &lmsc::AiGenerationService::cancelled);
    service.generate(request(path)); QTRY_COMPARE(transport.requests.size(), 1); service.cancel("hybrid");
    emit transport.completed({transport.requests.first().requestId, "{}", 1, 1}); QTest::qWait(30);
    QCOMPARE(ready.count(), 0); QCOMPARE(cancelled.count(), 1); QCOMPARE(service.status().state, lmsc::AiGenerationService::Status::Idle);
}
void HybridGenerationTest::configurationDuringAnalysisDoesNotStartSecondWorker() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; lmsc::HybridAiGenerationService service(&transport); QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    service.generate(request(path)); emit transport.configurationChanged(); QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(transport.requests.size(), 0);
    QVERIFY(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).warnings.join('\n').contains(QStringLiteral("设置")));
}
void HybridGenerationTest::planSignalCanDeleteService() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; QPointer<lmsc::HybridAiGenerationService> service = new lmsc::HybridAiGenerationService(&transport);
    connect(service, &lmsc::HybridAiGenerationService::planReady, this, [&service] { delete service.data(); });
    service->generate(request(path)); QTRY_VERIFY(service.isNull());
}
void HybridGenerationTest::planningProgressCanReplaceRequest() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; lmsc::HybridAiGenerationService service(&transport); QSignalSpy ready(&service, &lmsc::AiGenerationService::draftReady);
    bool replaced = false;
    connect(&service, &lmsc::AiGenerationService::progress, this, [&](const QString &job, int, const QString &stage) {
        if (!replaced && job == "hybrid" && stage.contains(QStringLiteral("一次整曲"))) { replaced = true; service.generate(request(path, "replacement")); }
    });
    service.generate(request(path)); QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).source.jobId, QString("replacement")); QCOMPARE(transport.requests.size(), 1);
}
void HybridGenerationTest::planningProgressCanDeleteService() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; QPointer<lmsc::HybridAiGenerationService> service = new lmsc::HybridAiGenerationService(&transport);
    connect(service, &lmsc::AiGenerationService::progress, this, [&service](const QString &, int, const QString &stage) {
        if (stage.contains(QStringLiteral("一次整曲"))) delete service.data();
    });
    service->generate(request(path)); QTRY_VERIFY(service.isNull()); QCOMPARE(transport.requests.size(), 0);
}
void HybridGenerationTest::cancelledTransportCanDeleteService() {
    QTemporaryDir dir; const auto path = dir.filePath("song.pcm"); QVERIFY(writePcm(path));
    Transport transport; transport.hold = true;
    QPointer<lmsc::HybridAiGenerationService> service = new lmsc::HybridAiGenerationService(&transport);
    service->generate(request(path)); QTRY_COMPARE(transport.requests.size(), 1);
    transport.onCancel = [&service] { delete service.data(); };
    service->skipPlanning("hybrid"); QVERIFY(service.isNull());
}
QTEST_GUILESS_MAIN(HybridGenerationTest)
#include "HybridGenerationTest.moc"
