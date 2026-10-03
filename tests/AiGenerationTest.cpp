#include "core/AiGenerationService.h"
#include "core/AiTextTransport.h"
#include "core/BeatmapPlayabilityValidator.h"
#include "core/MusicFeatureAnalyzer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSignalSpy>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <functional>

namespace {
constexpr double pi=3.14159265358979323846;
bool makePcm(const QString &path, double duration, int sampleRate=2000, int channels=2) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const int frames=qRound(duration*sampleRate);
    QByteArray buffer;
    buffer.reserve(65536);
    for (int i=0; i<frames; ++i) {
        const double time=i/double(sampleRate);
        const double phase=std::fmod(time,0.5);
        const double pulse=time>=1 && phase<0.045 ? .7*std::exp(-phase*35) : 0.0;
        const double sample=(.025+pulse)*std::sin(2*pi*110*time);
        const qint16 integer=static_cast<qint16>(sample*30000);
        for (int channel=0; channel<channels; ++channel) {
            char bytes[2]; qToLittleEndian<qint16>(integer,reinterpret_cast<uchar *>(bytes)); buffer.append(bytes,2);
        }
        if (buffer.size()>=64000) { if (file.write(buffer)!=buffer.size()) return false; buffer.clear(); }
    }
    return buffer.isEmpty() || file.write(buffer)==buffer.size();
}

QJsonObject validPlan(const QJsonObject &input) {
    const auto constraints=input.value("constraints").toObject();
    const auto allowed=constraints.value("allowedTypes").toArray();
    const bool hits=allowed.contains("directional") || allowed.contains("dot");
    QJsonArray sections;
    for (const auto &block:input.value("music").toObject().value("blocks").toArray())
        sections.append(QJsonObject{{"segmentId",block.toObject().value("segmentId")}, {"role","verse"},
            {"motifId","m1"}, {"targetNps",hits ? .5 : 0.0}, {"variation",.1}, {"intent","play"}});
    return {{"schemaVersion",1}, {"summary",QStringLiteral("根据本地强弱和起音规划左右交替动作")},
        {"motifs",QJsonArray{QJsonObject{{"id","m1"},{"description","顺手交替"}}}}, {"sections",sections}};
}
QJsonObject validSegment(const QJsonObject &input) {
    const auto constraints=input.value("constraints").toObject();
    const auto allowed=constraints.value("allowedTypes").toArray();
    const auto music=input.value("music").toObject();
    const double start=music.value("startBeat").toDouble();
    const auto anchors=music.value("anchors").toArray();
    QJsonArray objects;
    QStringList hits;
    QMap<qint64,QString> boundaries;
    QString rest;
    for (const auto &item:anchors) {
        const auto anchor=item.toObject();
        const QString kind=anchor.value("kind").toString();
        const double relative=anchor.value("beat").toDouble()-start;
        if (kind=="hit" && relative>=2-1e-6 && relative<=4+1e-6) hits.append(anchor.value("id").toString());
        if (kind=="rest" && rest.isEmpty()) rest=anchor.value("id").toString();
        if (kind=="boundary") boundaries.insert(qRound64(relative*1000),anchor.value("id").toString());
    }
    if (allowed.contains("directional") && !hits.isEmpty())
        objects.append(QJsonObject{{"kind","directional"},{"anchorId",hits.first()},{"hand","left"},{"x",1},{"y",1},{"direction",1}});
    if (allowed.contains("dot") && !hits.isEmpty()) {
        const QString anchor=allowed.contains("directional") && hits.size()>1 ? hits.last() : hits.first();
        objects.append(QJsonObject{{"kind","dot"},{"anchorId",anchor},{"hand","right"},{"x",2},{"y",1}});
    }
    if (allowed.contains("bomb") && !rest.isEmpty())
        objects.append(QJsonObject{{"kind","bomb"},{"anchorId",rest},{"x",0},{"y",0}});
    // Walls on alternate 8-second blocks respect the fixed 10-second separation.
    if (allowed.contains("wall") && qRound(music.value("startSeconds").toDouble())%16==0
            && boundaries.contains(0) && boundaries.contains(2000))
        objects.append(QJsonObject{{"kind","wall"},{"startAnchor",boundaries.value(0)},
            {"endAnchor",boundaries.value(2000)},{"x",3},{"y",0},{"width",1},{"height",5}});
    return {{"schemaVersion",1},{"segmentId",music.value("segmentId")},
            {"motifId",input.value("plan").toObject().value("motifId")},{"objects",objects}};
}

QJsonObject fourNoteSegment(const QJsonObject &input) {
    auto response=validSegment(input); QJsonArray notes;
    for (const auto &value : input.value("music").toObject().value("anchors").toArray()) {
        const auto anchor=value.toObject();
        if (anchor.value("kind").toString()!="hit") continue;
        const double relative=anchor.value("beat").toDouble()-input.value("music").toObject().value("startBeat").toDouble();
        if (relative<2-1e-6) continue;
        notes.append(QJsonObject{{"kind","directional"},{"anchorId",anchor.value("id")},
            {"hand","left"},{"x",1},{"y",1},{"direction",1}});
        if (notes.size()==4) break;
    }
    response.insert("objects",notes); return response;
}

class FakeTransport final : public lmsc::AiTextTransport {
public:
    using AiTextTransport::AiTextTransport;
    bool available=true, hold=false, networkFailure=false;
    QVector<lmsc::AiTextRequest> requests;
    QStringList cancellations;
    std::function<QString(const lmsc::AiTextRequest &, const QJsonObject &)> responder;
    std::function<lmsc::AiFailure(const lmsc::AiTextRequest &, const QJsonObject &)> failureResponder;
    lmsc::AiOutputPolicy policy;
    bool isAvailable() const override { return available; }
    void configure(const lmsc::AppPreferences &) override {}
    lmsc::AiOutputPolicy outputPolicy() const override { return policy; }
    void complete(const lmsc::AiTextRequest &request) override {
        requests.append(request);
        if (hold) return;
        const auto input=QJsonDocument::fromJson(request.userPrompt.toUtf8()).object();
        const QString answer=responder ? responder(request,input)
            : QString::fromUtf8(QJsonDocument(input.value("stage").toString()=="plan" ? validPlan(input) : validSegment(input)).toJson(QJsonDocument::Compact));
        auto failure=failureResponder ? failureResponder(request,input) : lmsc::AiFailure{};
        failure.requestId=request.requestId; failure.jobId=request.jobId;
        QTimer::singleShot(0,this,[this,request,answer,failure] {
            if (!failure.category.isEmpty()) { emit failureInfo(failure); emit failed(request.requestId,QStringLiteral("模拟暂时失败")); }
            else if (networkFailure) emit failed(request.requestId,QStringLiteral("模拟网络失败"));
            else emit completed({request.requestId,answer,10,20});
        });
    }
    void cancel(const QString &id) override { cancellations.append(id); }
};
void accelerateRecovery(lmsc::LlmAiGenerationService &service) {
    QObject::connect(&service,&lmsc::AiGenerationService::progress,&service,[&service] {
        if (service.status().recovering) QTimer::singleShot(0,&service,[&service] {
            auto *timer=service.findChild<QTimer *>("aiGenerationRecoveryTimer");
            if (timer && timer->isActive()) timer->start(0);
        });
    });
}
}

class AiGenerationTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void fiveConservativeProfiles();
    void fullSongBeyondThreeMinutesAndTimeMap();
    void sparsePercussionUsesMusicalActiveSpans();
    void generationAtEveryDifficulty_data();
    void generationAtEveryDifficulty();
    void fullSongGenerationBeyondThreeMinutes();
    void generationOptions_data();
    void generationOptions();
    void analysisOnlyUsesLlmPlanningWithoutObjects();
    void analysisOnlyAllowsNoObjectTypes();
    void strictAnchorsAndFields();
    void crossSegmentBothHandsAreValidated();
    void bombsAvoidShortHandConnectionsButAllowLongRests();
    void repeatedMotifAndHandContextAreSupplied();
    void changedRepeatedMotifIsRepaired();
    void motifComparisonSeparatesMusicalAdaptations();
    void repeatsUseFixedBestRepresentatives();
    void seventhOf21SegmentsKeepsBestSafeCandidate_data();
    void seventhOf21SegmentsKeepsBestSafeCandidate();
    void qualityRepairsRespectWholeTaskBudget();
    void qualityCandidateSurvivesNetworkPause();
    void hazardOnlySegmentDoesNotBecomeActionReference();
    void planJsonIsRepairedOnce();
    void segmentRepairIsBoundedWithoutFallback_data();
    void segmentRepairIsBoundedWithoutFallback();
    void cancellationAndStaleResponses();
    void destroyedTransportAndServiceAreSafe();
    void requestAndWholeTaskTimeouts();
    void networkFailuresDoNotRetry();
    void resumePreservesValidatedSegmentsAndRejectsLateReplies();
    void completedAnalysisIsReusedForGeneration();
    void emptyOrUnknownOptionsAreRejected();
    void completedAndCancelledTasksReleaseAudioLease();
    void truncationRaisesBudgetAndRetainsIt();
    void sixTransientFailuresFinish21Segments();
    void recoveryLimitAndManualContinue();
    void taskRecoveryBudgetSurvivesResume();
    void truncationWithoutLargerBudgetPauses_data();
    void truncationWithoutLargerBudgetPauses();
    void nonTransientFailuresNeverRecover_data();
    void nonTransientFailuresNeverRecover();
    void recoveryCanBeCancelledReconfiguredAndTimedOut_data();
    void recoveryCanBeCancelledReconfiguredAndTimedOut();
    void bestCandidateSurvivesAutomaticRecovery();
    void manualContinueRaisesTruncatedBudget();
private:
    QTemporaryDir temporary;
    QString shortPcm,longPcm,twentyOnePcm;
    lmsc::GenerationRequest request(double duration=24.0) const;
};

lmsc::GenerationRequest AiGenerationTest::request(double duration) const {
    lmsc::GenerationRequest result;
    result.jobId="job-1"; result.documentId="new-song"; result.documentRevision=1; result.audioRevision=1;
    result.difficultyId="difficulty-1";
    result.profile=lmsc::DifficultyProfile::forName("Expert");
    result.audio.path=duration==168 ? twentyOnePcm : duration>30 ? longPcm : shortPcm;
    result.audio.sourcePath="test-only-local-song.ogg";
    result.audio.revision=1; result.audio.sampleRate=2000; result.audio.channels=2; result.audio.durationSeconds=duration;
    result.timeMap.configure(120,0);
    return result;
}
void AiGenerationTest::initTestCase() {
    QVERIFY(temporary.isValid());
    shortPcm=temporary.filePath("short.pcm"); longPcm=temporary.filePath("long.pcm");
    QVERIFY(makePcm(shortPcm,24)); QVERIFY(makePcm(longPcm,188));
    twentyOnePcm=temporary.filePath("21-segments.pcm"); QVERIFY(makePcm(twentyOnePcm,168));
    qRegisterMetaType<lmsc::GenerationDraft>(); qRegisterMetaType<lmsc::AiTextResult>();
}
void AiGenerationTest::fiveConservativeProfiles() {
    const QStringList names{"Easy","Normal","Hard","Expert","ExpertPlus"};
    double previousPeak=0,previousGap=1,previousSpeed=0;
    for (int i=0;i<names.size();++i) {
        const auto profile=lmsc::DifficultyProfile::forName(names[i]);
        QCOMPARE(profile.name,names[i]); QCOMPARE(profile.rank,1+i*2);
        QVERIFY(profile.maxPeakNps>previousPeak); QVERIFY(profile.minSameHandGapSeconds<previousGap);
        QVERIFY(profile.maxConnectionSpeed>previousSpeed);
        previousPeak=profile.maxPeakNps; previousGap=profile.minSameHandGapSeconds; previousSpeed=profile.maxConnectionSpeed;
    }
    QCOMPARE(lmsc::DifficultyProfile::forName("Easy").subdivision,1);
    QCOMPARE(lmsc::DifficultyProfile::forName("Hard").subdivision,2);
    QCOMPARE(lmsc::DifficultyProfile::forName("ExpertPlus").subdivision,4);
}
void AiGenerationTest::fullSongBeyondThreeMinutesAndTimeMap() {
    auto source=request(188);
    QVERIFY(source.timeMap.configure(120,0,{{200,100}}));
    lmsc::MusicAnalysis analysis; QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source,&analysis,&error),qPrintable(error));
    QCOMPARE(analysis.durationSeconds,188.0); QCOMPARE(analysis.audioFingerprint.size(),64);
    QVERIFY(analysis.activeSeconds>180); QVERIFY(analysis.segments.size()>20);
    bool after180=false;
    for (const auto &anchor:analysis.anchors) {
        QVERIFY(std::abs(anchor.seconds-source.timeMap.beatToSeconds(anchor.beat))<1e-8);
        QVERIFY(anchor.beat>=0);
        if (anchor.kind==lmsc::MusicAnchorKind::Hit && anchor.seconds>180) after180=true;
    }
    QVERIFY(after180);
    QCOMPARE(analysis.segments.last().endSeconds,188.0);
    for (const auto &segment:analysis.segments) QVERIFY(segment.anchors.size()<=128);
}
void AiGenerationTest::sparsePercussionUsesMusicalActiveSpans() {
    const QString path=temporary.filePath("sparse-drums.pcm");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray bytes(24*2000*2*2,0);
    for (int i=0;i<24*2000;++i) {
        const double time=i/2000.0,phase=std::fmod(time,1.0);
        if (time<1 || (time>=10 && time<15) || phase>=.04) continue;
        const qint16 sample=static_cast<qint16>(25000*std::exp(-phase*35)*std::sin(2*pi*110*time));
        for (int channel=0;channel<2;++channel)
            qToLittleEndian<qint16>(sample,reinterpret_cast<uchar *>(bytes.data()+i*4+channel*2));
    }
    QCOMPARE(file.write(bytes),qint64(bytes.size())); file.close();
    auto source=request(); source.audio.path=path; source.profile=lmsc::DifficultyProfile::forName("Easy");
    lmsc::MusicAnalysis analysis; QString error;
    QVERIFY2(lmsc::MusicFeatureAnalyzer::analyze(source,&analysis,&error),qPrintable(error));
    QVERIFY(analysis.activeSeconds>15); QVERIFY(analysis.activeSeconds<20);
    QVector<lmsc::BeatObject> notes;
    for (const auto &anchor:analysis.anchors) if (anchor.kind==lmsc::MusicAnchorKind::Hit) {
        lmsc::BeatObject note; note.beat=anchor.beat; note.x=1; note.y=1; note.color=0; note.direction=notes.size()%2;
        notes.append(note);
    }
    QVERIFY(notes.size()>=17); QStringList errors;
    QVERIFY2(lmsc::BeatmapPlayabilityValidator::validateObjects(notes,source,analysis,&errors),qPrintable(errors.join('\n')));
    QVERIFY(lmsc::BeatmapPlayabilityValidator::metrics(notes,source.timeMap,analysis.activeSeconds).averageNps<1.4);
}
void AiGenerationTest::generationAtEveryDifficulty_data() {
    QTest::addColumn<QString>("difficulty");
    for (const auto &name:QStringList{"Easy","Normal","Hard","Expert","ExpertPlus"})
        QTest::newRow(qPrintable(name))<<name;
}
void AiGenerationTest::generationAtEveryDifficulty() {
    QFETCH(QString,difficulty);
    FakeTransport transport; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    auto source=request(); source.profile=lmsc::DifficultyProfile::forName(difficulty); service.generate(source);
    QTRY_VERIFY(ready.count() || failed.count());
    QVERIFY2(failed.isEmpty(),failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QCOMPARE(draft.source.profile.name,difficulty); QCOMPARE(draft.source.profile.rank,source.profile.rank);
    QVERIFY(draft.metrics.peakNps<=source.profile.maxPeakNps);
}
void AiGenerationTest::fullSongGenerationBeyondThreeMinutes() {
    FakeTransport transport; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    auto source=request(188); service.generate(source); QTRY_VERIFY(ready.count() || failed.count());
    QVERIFY2(failed.isEmpty(),failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QVERIFY(transport.requests.size()>=25);
    bool after180=false;
    for (const auto &object:draft.objects) after180 |= source.timeMap.beatToSeconds(object.beat)>180;
    QVERIFY(after180);
}
void AiGenerationTest::generationOptions_data() {
    QTest::addColumn<int>("types");
    QTest::newRow("directional")<<int(lmsc::DirectionalType);
    QTest::newRow("dot")<<int(lmsc::DotType);
    QTest::newRow("bomb")<<int(lmsc::BombType);
    QTest::newRow("wall")<<int(lmsc::WallType);
    QTest::newRow("all")<<15;
}
void AiGenerationTest::generationOptions() {
    QFETCH(int,types);
    FakeTransport transport;
    lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    auto source=request(); source.allowedTypes=lmsc::GeneratedTypes(types);
    service.generate(source);
    QTRY_VERIFY(ready.count()==1 || failed.count()==1);
    QVERIFY2(failed.isEmpty(),failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QVERIFY(!draft.objects.isEmpty()); QCOMPARE(int(draft.source.allowedTypes),types);
    for (const auto &object:draft.objects) {
        const int bit=object.kind==lmsc::ObjectKind::Wall ? lmsc::WallType
            : object.kind==lmsc::ObjectKind::Bomb ? lmsc::BombType
            : object.direction==8 ? lmsc::DotType : lmsc::DirectionalType;
        QVERIFY(types&bit);
    }
    QVERIFY(transport.requests.size()>=2);
    for (const auto &message:transport.requests) {
        QCOMPARE(message.timeoutMs,600000); QVERIFY(!message.userPrompt.contains(shortPcm));
        QVERIFY(!message.userPrompt.contains(source.audio.sourcePath)); QVERIFY(!message.outputSchema.isEmpty());
    }
}
void AiGenerationTest::analysisOnlyUsesLlmPlanningWithoutObjects() {
    FakeTransport transport; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);
    auto source=request(); source.analysisOnly=true; service.generate(source);
    QTRY_COMPARE(ready.count(),1);
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QVERIFY(!draft.summary.isEmpty()); QVERIFY(draft.objects.isEmpty()); QCOMPARE(transport.requests.size(),1);
}
void AiGenerationTest::analysisOnlyAllowsNoObjectTypes() {
    FakeTransport transport; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    QSignalSpy progress(&service,&lmsc::AiGenerationService::progress);
    auto source=request(); source.analysisOnly=true; source.allowedTypes={}; service.generate(source);
    QTRY_VERIFY(ready.count() || failed.count());
    QVERIFY2(failed.isEmpty(),failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QVERIFY(!draft.summary.isEmpty()); QVERIFY(draft.objects.isEmpty());
    QVERIFY(draft.source.analysisOnly); QCOMPARE(int(draft.source.allowedTypes),0);
    QCOMPARE(transport.requests.size(),1);
    QCOMPARE(progress.last().at(2).toString(),QStringLiteral("音乐分析完成"));
    source.allowedTypes=lmsc::GeneratedTypes(16); service.generate(source);
    QCOMPARE(failed.count(),1); QCOMPARE(transport.requests.size(),1);
    source.analysisOnly=false; source.allowedTypes={}; service.generate(source);
    QCOMPARE(failed.count(),2); QCOMPARE(transport.requests.size(),1);
}
void AiGenerationTest::strictAnchorsAndFields() {
    const auto source=request(); lmsc::MusicAnalysis analysis; QString error;
    QVERIFY(lmsc::MusicFeatureAnalyzer::analyze(source,&analysis,&error));
    const QJsonObject input{{"constraints",QJsonObject{{"allowedTypes",QJsonArray{"directional"}}}},
        {"music",analysis.segmentEvidence(0)},{"plan",QJsonObject{{"motifId","m1"}}}};
    QJsonObject json=validSegment(input); QVector<lmsc::BeatObject> output; QStringList errors;
    QVERIFY(lmsc::BeatmapPlayabilityValidator::parseAndValidate(json,source,analysis,0,"m1",{},&output,&errors));
    auto rows=json.value("objects").toArray(); auto row=rows[0].toObject(); row.insert("anchorId","invented-time"); rows[0]=row; json.insert("objects",rows);
    QVERIFY(!lmsc::BeatmapPlayabilityValidator::parseAndValidate(json,source,analysis,0,"m1",{},&output,&errors));
    QVERIFY(output.isEmpty()); QVERIFY(errors.join(' ').contains(QStringLiteral("锚点")));
    json=validSegment(input); rows=json.value("objects").toArray(); row=rows[0].toObject(); row.insert("beat",123.0); rows[0]=row; json.insert("objects",rows);
    QVERIFY(!lmsc::BeatmapPlayabilityValidator::parseAndValidate(json,source,analysis,0,"m1",{},&output,&errors));
    json=validSegment(input); rows=json.value("objects").toArray(); row=rows[0].toObject(); row.insert("x",1.5); rows[0]=row; json.insert("objects",rows);
    QVERIFY(!lmsc::BeatmapPlayabilityValidator::parseAndValidate(json,source,analysis,0,"m1",{},&output,&errors));
}
void AiGenerationTest::crossSegmentBothHandsAreValidated() {
    auto source=request(); source.profile=lmsc::DifficultyProfile::forName("ExpertPlus");
    lmsc::MusicAnalysis analysis; analysis.durationSeconds=24; analysis.activeSeconds=24;
    analysis.segments.append({"s0","","","",16,32,8,16,1,8,0,{0},{}});
    analysis.anchors.append({"h16",lmsc::MusicAnchorKind::Hit,8,16,1,1}); analysis.anchorIndex.insert("h16",0);
    QVector<lmsc::BeatObject> previous;
    for (int hand=0;hand<2;++hand) { lmsc::BeatObject note; note.beat=15.75; note.color=hand; note.x=1+hand; note.y=1; note.direction=0; previous.append(note); }
    const QJsonObject json{{"schemaVersion",1},{"segmentId","s0"},{"motifId","m1"},{"objects",QJsonArray{
        QJsonObject{{"kind","directional"},{"anchorId","h16"},{"hand","left"},{"x",1},{"y",1},{"direction",1}},
        QJsonObject{{"kind","directional"},{"anchorId","h16"},{"hand","right"},{"x",2},{"y",1},{"direction",1}}}}};
    QVector<lmsc::BeatObject> fresh; QStringList errors;
    QVERIFY(!lmsc::BeatmapPlayabilityValidator::parseAndValidate(json,source,analysis,0,"m1",previous,&fresh,&errors));
    QVERIFY(errors.join(' ').contains(QStringLiteral("左手"))); QVERIFY(errors.join(' ').contains(QStringLiteral("右手")));
}
void AiGenerationTest::bombsAvoidShortHandConnectionsButAllowLongRests() {
    auto source=request(); source.allowedTypes=lmsc::DirectionalType | lmsc::BombType;
    lmsc::MusicAnalysis analysis; analysis.durationSeconds=24; analysis.activeSeconds=24;
    for (int hand=0;hand<2;++hand) {
        lmsc::BeatObject first; first.beat=2; first.color=hand; first.x=hand ? 3 : 0;
        first.y=0; first.direction=hand ? 2 : 3;
        lmsc::BeatObject next; next.beat=3.6; next.color=hand; next.x=hand ? 2 : 1;
        next.y=2; next.direction=0;
        lmsc::BeatObject bomb; bomb.kind=lmsc::ObjectKind::Bomb; bomb.beat=2.8;
        bomb.x=next.x; bomb.y=1;
        QStringList errors;
        // Each cut is 0.4 seconds from the bomb and clear in space. Only the
        // time-clipped connection between the two cuts intersects its radius.
        QVERIFY(!lmsc::BeatmapPlayabilityValidator::validateObjects({first,next,bomb},source,analysis,&errors));
        QVERIFY2(errors.join(' ').contains(QStringLiteral("移动路径")),qPrintable(errors.join('\n')));
        QVERIFY(!errors.join(' ').contains(QStringLiteral("挥刀路径")));
        bomb.x=hand ? 0 : 3;
        QVERIFY2(lmsc::BeatmapPlayabilityValidator::validateObjects({first,next,bomb},source,analysis,&errors),qPrintable(errors.join('\n')));
        // A two-second rest lets the player return to neutral; do not assume
        // that their hand spends the entire rest on the straight connection.
        next.beat=6; bomb.beat=4; bomb.x=next.x;
        QVERIFY2(lmsc::BeatmapPlayabilityValidator::validateObjects({first,next,bomb},source,analysis,&errors),qPrintable(errors.join('\n')));
    }
}
void AiGenerationTest::repeatedMotifAndHandContextAreSupplied() {
    FakeTransport transport; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_VERIFY(ready.count() || failed.count());
    QVERIFY2(failed.isEmpty(),failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    bool reference=false,leftState=false;
    for (const auto &message:transport.requests) {
        const auto input=QJsonDocument::fromJson(message.userPrompt.toUtf8()).object();
        reference |= input.contains("motifReference");
        leftState |= input.value("handContext").toObject().value("left").toObject().value("hasPrevious").toBool();
    }
    QVERIFY(leftState); QVERIFY(reference);
}
void AiGenerationTest::changedRepeatedMotifIsRepaired() {
    FakeTransport transport; bool changed=false,repaired=false;
    transport.responder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        QJsonObject response=input.value("stage").toString()=="plan" ? validPlan(input) : validSegment(input);
        if (input.contains("motifReference") && !changed) {
            auto rows=response.value("objects").toArray(); auto row=rows[0].toObject(); row.insert("direction",0);
            rows[0]=row; response.insert("objects",rows); changed=true;
        } else if (changed && input.contains("validationErrors")) repaired=true;
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_VERIFY(ready.count() || failed.count());
    QVERIFY2(failed.isEmpty(),failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QVERIFY(changed); QVERIFY(repaired);
}
void AiGenerationTest::motifComparisonSeparatesMusicalAdaptations() {
    QVector<lmsc::BeatObject> reference;
    for (int i=0;i<14;++i) {
        lmsc::BeatObject note; note.beat=i; note.color=i%2; note.x=note.color ? 2 : 1;
        note.y=1; note.direction=(i/2)%2; reference.append(note);
    }
    auto shifted=reference;
    for (auto &note:shifted) note.beat+=33;
    auto comparison=lmsc::BeatmapPlayabilityValidator::compareMotifs(reference,0,shifted,32);
    QVERIFY(comparison.comparable); QCOMPARE(comparison.actionDifference,0.0);
    QVERIFY(comparison.rhythmCoverage<1); QCOMPARE(comparison.positionDifference,0.0);
    const auto reduced=reference.mid(0,8);
    comparison=lmsc::BeatmapPlayabilityValidator::compareMotifs(reference,0,reduced,0);
    QCOMPARE(comparison.actionDifference,0.0); QCOMPARE(comparison.referenceNotes,14); QCOMPARE(comparison.currentNotes,8);
    QVERIFY(std::abs(comparison.countDifference-6.0/14)<1e-9);
    auto moved=reference;
    for (int i=0;i<6;++i) moved[i].x=moved[i].color ? 3 : 0;
    comparison=lmsc::BeatmapPlayabilityValidator::compareMotifs(reference,0,moved,0);
    QCOMPARE(comparison.actionDifference,0.0); QCOMPARE(comparison.rhythmCoverage,1.0);
    QVERIFY(std::abs(comparison.positionDifference-6.0/14)<1e-9);
    auto changed=reference;
    for (auto &note:changed) note.direction=2;
    comparison=lmsc::BeatmapPlayabilityValidator::compareMotifs(reference,0,changed,0);
    QCOMPARE(comparison.actionDifference,1.0); QVERIFY(!comparison.differences.isEmpty());
    changed=reference; for (auto &note:changed) note.color=1-note.color;
    QVERIFY(lmsc::BeatmapPlayabilityValidator::compareMotifs(reference,0,changed,0).actionDifference>0);
    // JSON order of simultaneous hands must not alter the theme.
    auto a=reference[0],b=reference[1]; b.beat=a.beat;
    QCOMPARE(lmsc::BeatmapPlayabilityValidator::compareMotifs({a,b},0,{b,a},0).actionDifference,0.0);
    b.direction=3;
    QCOMPARE(lmsc::BeatmapPlayabilityValidator::compareMotifs({a},0,{b},0).actionDifference,1.0);
    QVERIFY(!lmsc::BeatmapPlayabilityValidator::compareMotifs({},0,reference,0).comparable);
    a.kind=lmsc::ObjectKind::Bomb; b.kind=lmsc::ObjectKind::Wall;
    QVERIFY(!lmsc::BeatmapPlayabilityValidator::compareMotifs({a,b},0,{a},0).comparable);
}
void AiGenerationTest::repeatsUseFixedBestRepresentatives() {
    auto segment=[](double angle) {
        lmsc::MusicSegment result; result.endBeat=16; result.energy=.5;
        result.fingerprint={std::cos(angle*pi/180),std::sin(angle*pi/180)}; return result;
    };
    lmsc::MusicAnalysis analysis; analysis.segments={segment(0),segment(18),segment(36)};
    analysis.identifyRepeats();
    QCOMPARE(analysis.segments[1].repeatReference,0);
    QCOMPARE(analysis.segments[2].repeatReference,-1); // Close to B, but not the fixed A reference.
    analysis.segments={segment(0),segment(30),segment(17)}; analysis.identifyRepeats();
    QCOMPARE(analysis.segments[2].repeatReference,1); // The best eligible group, not the first.
    QCOMPARE(analysis.segmentEvidence(2).value("repeatReferenceSegment").toInt(),2);
    QCOMPARE(analysis.segmentEvidence(2).value("repeatConfidence").toDouble(),analysis.segments[2].repeatConfidence);
    analysis.segments[2].energy=1; analysis.identifyRepeats(); QCOMPARE(analysis.segments[2].repeatReference,-1);
    analysis.segments[2]=segment(17); analysis.segments[2].endBeat=15; analysis.identifyRepeats();
    QCOMPARE(analysis.segments[2].repeatReference,-1);
}
void AiGenerationTest::seventhOf21SegmentsKeepsBestSafeCandidate_data() {
    QTest::addColumn<bool>("worse"); QTest::addColumn<bool>("invalidJson");
    QTest::newRow("worse-then-invalid-anchor") << true << false;
    QTest::newRow("tie-then-invalid-json") << false << true;
}
void AiGenerationTest::seventhOf21SegmentsKeepsBestSafeCandidate() {
    QFETCH(bool,worse); QFETCH(bool,invalidJson);
    FakeTransport transport; int seventhCalls=0; bool feedback=false,referenceSupplied=true;
    transport.responder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        auto response=input.value("stage").toString()=="plan" ? validPlan(input) : fourNoteSegment(input);
        if (input.value("music").toObject().value("segmentId").toString()=="s6") {
            ++seventhCalls;
            referenceSupplied &= input.contains("motifReference");
            auto rows=response.value("objects").toArray();
            if (seventhCalls==1) { auto row=rows[0].toObject(); row["direction"]=0; row["x"]=0; rows[0]=row; }
            else if (seventhCalls==2) {
                feedback=input.contains("previousCandidate") && input.value("qualityFeedback").toObject().value("differences").isArray();
                for (int i=0;i<(worse ? rows.size() : 1);++i) { auto row=rows[i].toObject(); row["direction"]=0; rows[i]=row; }
            } else {
                if (invalidJson) return QStringLiteral("invalid JSON");
                auto row=rows[0].toObject(); row["anchorId"]="hallucinated"; rows[0]=row;
            }
            response["objects"]=rows;
        }
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    lmsc::LlmAiGenerationService service(&transport); QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    auto source=request(168); source.profile=lmsc::DifficultyProfile::forName("Easy");
    service.generate(source); QTRY_VERIFY(ready.count() || failed.count());
    QVERIFY2(failed.isEmpty(),failed.isEmpty()? "" : qPrintable(failed.first().at(1).toString()));
    QCOMPARE(service.status().totalSegments,21); QCOMPARE(service.status().completedSegments,21);
    QCOMPARE(seventhCalls,3); QVERIFY(feedback); QVERIFY(referenceSupplied);
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first());
    QVERIFY(draft.hasThemeWarnings); QVERIFY(draft.warnings.join(' ').contains(QStringLiteral("乐句 7")));
    QCOMPARE(draft.objects.size(),84);
    for (int i=0;i<24;++i) { QCOMPARE(draft.objects[i].direction,1); QCOMPARE(draft.objects[i].x,1); }
    QCOMPARE(draft.objects[24].direction,0); QCOMPARE(draft.objects[24].x,0);
    QCOMPARE(draft.objects[25].direction,1); QCOMPARE(service.status().percent,100);
}
void AiGenerationTest::qualityRepairsRespectWholeTaskBudget() {
    FakeTransport transport; int repeatCalls=0;
    transport.responder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        auto response=input.value("stage").toString()=="plan" ? validPlan(input) : fourNoteSegment(input);
        if (input.contains("motifReference")) {
            ++repeatCalls; auto rows=response.value("objects").toArray(); auto row=rows[0].toObject(); row["direction"]=0; rows[0]=row; response["objects"]=rows;
        }
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    lmsc::LlmAiGenerationService service(&transport); QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);
    auto source=request(168); source.profile=lmsc::DifficultyProfile::forName("Easy"); service.generate(source); QTRY_COMPARE(ready.count(),1);
    QCOMPARE(transport.requests.size(),32); // One plan + 21 segments + the task's ten repairs.
    QVERIFY(repeatCalls>10); QVERIFY(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).hasThemeWarnings);
}
void AiGenerationTest::qualityCandidateSurvivesNetworkPause() {
    FakeTransport transport; bool injectNetwork=false; int badCalls=0;
    transport.responder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        auto response=input.value("stage").toString()=="plan" ? validPlan(input) : fourNoteSegment(input);
        if (input.contains("motifReference") && input.value("music").toObject().value("segmentId").toString()=="s2") {
            ++badCalls; auto rows=response.value("objects").toArray(); auto row=rows[0].toObject(); row["direction"]=0; rows[0]=row; response["objects"]=rows;
            if (badCalls==2) { transport.networkFailure=true; injectNetwork=true; }
            if (badCalls>=3) { row=rows[0].toObject(); row["anchorId"]="invalid"; rows[0]=row; response["objects"]=rows; }
        }
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    lmsc::LlmAiGenerationService service(&transport); QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_COMPARE(failed.count(),1); QVERIFY(injectNetwork); QVERIFY(service.status().resumable);
    const int pendingIndex=transport.requests.size()-1;
    const auto pending=transport.requests.last(); const auto input=QJsonDocument::fromJson(pending.userPrompt.toUtf8()).object();
    QCOMPARE(service.status().completedSegments,2); transport.networkFailure=false; service.resume("job-1");
    QTRY_COMPARE(ready.count(),1); QCOMPARE(badCalls,4);
    const auto retryInput=QJsonDocument::fromJson(transport.requests[pendingIndex+1].userPrompt.toUtf8()).object();
    QCOMPARE(retryInput.value("previousCandidate"),input.value("previousCandidate"));
    QCOMPARE(retryInput.value("qualityFeedback"),input.value("qualityFeedback"));
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first()); QVERIFY(draft.hasThemeWarnings);
    QCOMPARE(draft.objects[8].direction,0); QCOMPARE(draft.objects[0].direction,1);
}
void AiGenerationTest::hazardOnlySegmentDoesNotBecomeActionReference() {
    FakeTransport transport; bool sawHazard=false,sawFollowing=false,hadFalseReference=false;
    QString hazardGroup,followingGroup;
    transport.responder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        auto response=input.value("stage").toString()=="plan" ? validPlan(input) : validSegment(input);
        if (input.value("stage").toString()=="plan") {
            auto motifs=response.value("motifs").toArray();
            motifs.append(QJsonObject{{"id","m2"},{"description","障碍后进入击打主题"}}); response["motifs"]=motifs;
            auto sections=response.value("sections").toArray();
            for (int i=1;i<sections.size();++i) { auto row=sections[i].toObject(); row["motifId"]="m2"; sections[i]=row; }
            response["sections"]=sections;
        }
        const auto music=input.value("music").toObject();
        if (music.value("segmentId").toString()=="s1") {
            QJsonArray hazards;
            for (const auto &row : response.value("objects").toArray())
                if (row.toObject().value("kind").toString()=="bomb") hazards.append(row);
            sawHazard=!hazards.isEmpty(); response["objects"]=hazards;
            hazardGroup=music.value("repeatGroup").toString();
        }
        if (music.value("segmentId").toString()=="s2") {
            sawFollowing=true; hadFalseReference=input.contains("motifReference");
            followingGroup=music.value("repeatGroup").toString();
        }
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    lmsc::LlmAiGenerationService service(&transport); QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);
    auto source=request(); source.allowedTypes=lmsc::DirectionalType|lmsc::BombType; service.generate(source);
    QTRY_COMPARE(ready.count(),1); QVERIFY(sawHazard && sawFollowing); QCOMPARE(hazardGroup,followingGroup);
    QVERIFY(!hadFalseReference);
    QVERIFY(!qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).hasThemeWarnings);
}
void AiGenerationTest::planJsonIsRepairedOnce() {
    FakeTransport transport; int calls=0;
    transport.responder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        if (!calls++) return QStringLiteral("not-json");
        return QString::fromUtf8(QJsonDocument(input.value("stage").toString()=="plan" ? validPlan(input) : validSegment(input)).toJson(QJsonDocument::Compact));
    };
    lmsc::LlmAiGenerationService service(&transport); QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);
    service.generate(request()); QTRY_COMPARE(ready.count(),1);
    QVERIFY(QJsonDocument::fromJson(transport.requests[1].userPrompt.toUtf8()).object().contains("validationErrors"));
}
void AiGenerationTest::segmentRepairIsBoundedWithoutFallback_data() {
    QTest::addColumn<bool>("wrongSide");
    QTest::newRow("unknown-anchor") << false;
    QTest::newRow("unsafe-hand-side") << true;
}
void AiGenerationTest::segmentRepairIsBoundedWithoutFallback() {
    QFETCH(bool,wrongSide);
    FakeTransport transport;
    transport.responder=[wrongSide](const lmsc::AiTextRequest &,const QJsonObject &input) {
        QJsonObject response=input.value("stage").toString()=="plan" ? validPlan(input) : validSegment(input);
        if (input.value("stage").toString()=="segment") {
            auto rows=response.value("objects").toArray(); auto row=rows[0].toObject();
            if (wrongSide) row.insert("x",2); else row.insert("anchorId","hallucinated");
            rows[0]=row; response.insert("objects",rows);
        }
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_COMPARE(failed.count(),1); QCOMPARE(ready.count(),0);
    QCOMPARE(transport.requests.size(),4); QVERIFY(failed.first().at(1).toString().contains(QStringLiteral("返修上限")));
}
void AiGenerationTest::cancellationAndStaleResponses() {
    FakeTransport transport; transport.hold=true; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed),cancelled(&service,&lmsc::AiGenerationService::cancelled);
    service.generate(request()); QTRY_COMPARE(transport.requests.size(),1);
    const auto old=transport.requests.first(); service.cancel("other-job"); QCOMPARE(cancelled.count(),0);
    service.cancel("job-1"); QCOMPARE(cancelled.count(),1); QVERIFY(transport.cancellations.contains(old.requestId));
    emit transport.completed({old.requestId,QString::fromUtf8(QJsonDocument(validPlan(QJsonDocument::fromJson(old.userPrompt.toUtf8()).object())).toJson()),1,1});
    QCOMPARE(ready.count(),0); QCOMPARE(failed.count(),0);
    transport.hold=false; auto next=request(); next.jobId="next-job"; service.generate(next);
    QTRY_COMPARE(ready.count(),1); QCOMPARE(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).source.jobId,QStringLiteral("next-job"));
    service.generate(request()); service.cancel("job-1"); QTest::qWait(100); QCOMPARE(ready.count(),1);
}
void AiGenerationTest::destroyedTransportAndServiceAreSafe() {
    auto *transport=new FakeTransport; transport->hold=true;
    lmsc::LlmAiGenerationService service(transport); QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_COMPARE(transport->requests.size(),1); delete transport;
    QTRY_COMPARE(failed.count(),1); QVERIFY(!service.isAvailable());
    FakeTransport other; auto *pending=new lmsc::LlmAiGenerationService(&other);
    pending->generate(request(188)); delete pending; QTest::qWait(50);
}
void AiGenerationTest::requestAndWholeTaskTimeouts() {
    FakeTransport transport; transport.hold=true; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_COMPARE(transport.requests.size(),1);
    const auto pending=transport.requests.last();
    QCOMPARE(pending.timeoutMs,600000);
    QVERIFY(!service.findChild<QTimer *>("aiGenerationRequestTimer"));
    emit transport.failed(pending.requestId,QStringLiteral("模拟传输超时"));
    QTRY_COMPARE(failed.count(),1); QVERIFY(!transport.cancellations.isEmpty());
    QVERIFY(service.status().resumable);
    service.generate(request(188)); auto *timer=service.findChild<QTimer *>("aiGenerationTaskTimer"); QVERIFY(timer); timer->start(1);
    QTRY_COMPARE(failed.count(),2); QTest::qWait(50);
}
void AiGenerationTest::networkFailuresDoNotRetry() {
    FakeTransport transport; transport.networkFailure=true; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed),ready(&service,&lmsc::AiGenerationService::draftReady);
    service.generate(request()); QTRY_COMPARE(failed.count(),1); QCOMPARE(transport.requests.size(),1); QCOMPARE(ready.count(),0);
}
void AiGenerationTest::emptyOrUnknownOptionsAreRejected() {
    FakeTransport transport; lmsc::LlmAiGenerationService service(&transport); QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed);
    auto source=request(); source.allowedTypes={}; service.generate(source); QCOMPARE(failed.count(),1);
    source.allowedTypes=lmsc::GeneratedTypes(16); service.generate(source); QCOMPARE(failed.count(),2);
    source.allowedTypes=lmsc::DirectionalType; source.profile.name="Unknown"; service.generate(source); QCOMPARE(failed.count(),3);
    QCOMPARE(transport.requests.size(),0);
}
void AiGenerationTest::completedAndCancelledTasksReleaseAudioLease() {
    FakeTransport transport; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);
    auto source=request(); auto lease=std::make_shared<int>(1); std::weak_ptr<int> weak=lease;
    source.audio.lease=lease; lease.reset(); service.generate(source); source.audio.lease.reset();
    QTRY_COMPARE(ready.count(),1); QVERIFY(!weak.expired()); // The preview owns its copy.
    ready.clear(); QVERIFY(!weak.expired()); service.discard(source.jobId); QTRY_VERIFY(weak.expired());
    source=request(188); lease=std::make_shared<int>(2); weak=lease;
    source.audio.lease=lease; lease.reset(); service.generate(source); source.audio.lease.reset(); service.cancel(source.jobId);
    QVERIFY(!weak.expired()); service.discard(source.jobId);
    QTRY_VERIFY(weak.expired());
}

void AiGenerationTest::resumePreservesValidatedSegmentsAndRejectsLateReplies() {
    FakeTransport transport; transport.hold=true; lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);
    const auto reply=[&](const lmsc::AiTextRequest &message) {
        const auto input=QJsonDocument::fromJson(message.userPrompt.toUtf8()).object();
        const auto output=input.value("stage").toString()=="plan" ? validPlan(input) : validSegment(input);
        emit transport.completed({message.requestId,QString::fromUtf8(QJsonDocument(output).toJson()),1,1});
    };
    service.generate(request()); QTRY_COMPARE(transport.requests.size(),1); reply(transport.requests.last());
    QTRY_COMPARE(transport.requests.size(),2); reply(transport.requests.last());
    QTRY_COMPARE(transport.requests.size(),3);
    const auto failedSegment=transport.requests.last();
    const auto failedInput=QJsonDocument::fromJson(failedSegment.userPrompt.toUtf8()).object();
    QCOMPARE(service.status().completedSegments,1);
    emit transport.failed(failedSegment.requestId,QStringLiteral("模拟连接失败"));
    QVERIFY(service.status().resumable); QCOMPARE(service.status().state,lmsc::AiGenerationService::Status::Paused);
    QTest::qWait(30); QCOMPARE(transport.requests.size(),3);
    service.resume("job-1"); QTRY_COMPARE(transport.requests.size(),4);
    const auto retry=transport.requests.last(); QVERIFY(retry.requestId!=failedSegment.requestId);
    const auto retryInput=QJsonDocument::fromJson(retry.userPrompt.toUtf8()).object();
    QCOMPARE(retryInput.value("music"),failedInput.value("music")); QCOMPARE(retryInput.value("handContext"),failedInput.value("handContext"));
    reply(failedSegment); QCOMPARE(ready.count(),0); QCOMPARE(service.status().completedSegments,1);
    transport.hold=false; reply(retry); QTRY_COMPARE(ready.count(),1);
    QCOMPARE(service.status().state,lmsc::AiGenerationService::Status::Completed);
    QCOMPARE(service.status().percent,100);
}
void AiGenerationTest::completedAnalysisIsReusedForGeneration() {
    FakeTransport transport; lmsc::LlmAiGenerationService service(&transport); QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);
    auto source=request(); source.analysisOnly=true; service.generate(source); QTRY_COMPARE(ready.count(),1);
    QCOMPARE(transport.requests.size(),1);
    source.analysisOnly=false; source.jobId="generation-from-analysis"; service.generate(source); QTRY_COMPARE(ready.count(),2);
    int plans=0;
    for (const auto &message:transport.requests) if (QJsonDocument::fromJson(message.userPrompt.toUtf8()).object().value("stage").toString()=="plan") ++plans;
    QCOMPARE(plans,1); QVERIFY(!qvariant_cast<lmsc::GenerationDraft>(ready.last().first()).objects.isEmpty());
}
void AiGenerationTest::truncationRaisesBudgetAndRetainsIt() {
    FakeTransport transport; transport.policy={8192,32768,false};
    transport.failureResponder=[](const lmsc::AiTextRequest &message,const QJsonObject &input) {
        lmsc::AiFailure failure;
        if (input.value("stage").toString()=="plan" && message.maxOutputTokens<32768) {
            failure.category="truncated"; failure.maxOutputTokens=message.maxOutputTokens;
        }
        return failure;
    };
    lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady), failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_COMPARE(ready.count(),1); QCOMPARE(failed.count(),0);
    QCOMPARE(transport.requests[0].maxOutputTokens,8192); QCOMPARE(transport.requests[1].maxOutputTokens,16384);
    for (int i=2;i<transport.requests.size();++i) QCOMPARE(transport.requests[i].maxOutputTokens,32768);
    QCOMPARE(transport.requests[0].userPrompt,transport.requests[1].userPrompt);
    QCOMPARE(transport.requests[1].userPrompt,transport.requests[2].userPrompt);
    QVERIFY(transport.requests[0].requestId!=transport.requests[1].requestId);
    QCOMPARE(service.status().percent,100); QVERIFY(!service.status().recovering);
}
void AiGenerationTest::sixTransientFailuresFinish21Segments() {
    auto source=request(168); source.profile=lmsc::DifficultyProfile::forName("Easy");
    FakeTransport baseline; lmsc::LlmAiGenerationService original(&baseline);
    QSignalSpy originalReady(&original,&lmsc::AiGenerationService::draftReady);
    original.generate(source); QTRY_COMPARE(originalReady.count(),1);
    const auto expected=qvariant_cast<lmsc::GenerationDraft>(originalReady.first().first());
    FakeTransport transport; QSet<QString> injected;
    transport.failureResponder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        lmsc::AiFailure failure; const QString id=input.value("music").toObject().value("segmentId").toString();
        if (QStringList{"s1","s4","s7","s10","s13","s16"}.contains(id) && !injected.contains(id)) {
            injected.insert(id); failure.category=injected.size()%3==0 ? "server" : injected.size()%3==1 ? "network" : "timeout";
            if (failure.category=="server") failure.httpStatus=503;
        }
        return failure;
    };
    lmsc::LlmAiGenerationService service(&transport); accelerateRecovery(service);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady), failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(source); QTRY_COMPARE(ready.count(),1); QCOMPARE(failed.count(),0);
    QCOMPARE(injected.size(),6); QCOMPARE(service.status().completedSegments,21); QCOMPARE(transport.requests.size(),28);
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first()); QCOMPARE(draft.objects.size(),expected.objects.size());
    for (int i=0;i<draft.objects.size();++i) {
        QCOMPARE(draft.objects[i].beat,expected.objects[i].beat); QCOMPARE(draft.objects[i].kind,expected.objects[i].kind);
        QCOMPARE(draft.objects[i].x,expected.objects[i].x); QCOMPARE(draft.objects[i].direction,expected.objects[i].direction);
    }
}
void AiGenerationTest::recoveryLimitAndManualContinue() {
    FakeTransport transport; transport.failureResponder=[](const lmsc::AiTextRequest &,const QJsonObject &) {
        lmsc::AiFailure failure; failure.category="network"; return failure;
    };
    lmsc::LlmAiGenerationService service(&transport); accelerateRecovery(service);
    QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed), ready(&service,&lmsc::AiGenerationService::draftReady);
    service.generate(request()); QTRY_COMPARE(failed.count(),1); QCOMPARE(transport.requests.size(),3);
    QCOMPARE(service.status().pauseCategory,QString("network")); QVERIFY(service.status().resumable);
    transport.failureResponder={}; service.resume("job-1"); QTRY_COMPARE(ready.count(),1);
    QCOMPARE(transport.requests[2].userPrompt,transport.requests[3].userPrompt);
}
void AiGenerationTest::taskRecoveryBudgetSurvivesResume() {
    FakeTransport transport; QSet<QString> injected;
    transport.failureResponder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        lmsc::AiFailure failure; const QString id=input.value("music").toObject().value("segmentId").toString();
        if (!id.isEmpty() && !injected.contains(id)) { injected.insert(id); failure.category="network"; }
        return failure;
    };
    lmsc::LlmAiGenerationService service(&transport); accelerateRecovery(service);
    QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed), ready(&service,&lmsc::AiGenerationService::draftReady);
    service.generate(request(168)); QTRY_COMPARE(failed.count(),1); QCOMPARE(injected.size(),11);
    QCOMPARE(service.status().completedSegments,10);
    service.resume("job-1"); QTRY_COMPARE(failed.count(),2); QCOMPARE(injected.size(),12);
    QCOMPARE(service.status().completedSegments,11); // Manual continuation does not renew the ten automatic recoveries.
    transport.failureResponder={}; service.resume("job-1"); QTRY_COMPARE(ready.count(),1);
}
void AiGenerationTest::truncationWithoutLargerBudgetPauses_data() {
    QTest::addColumn<int>("initial"); QTest::addColumn<int>("maximum");
    QTest::newRow("service-managed") << 0 << 0;
    QTest::newRow("already-at-cap") << 8192 << 8192;
}
void AiGenerationTest::truncationWithoutLargerBudgetPauses() {
    QFETCH(int,initial); QFETCH(int,maximum);
    FakeTransport transport; transport.policy={initial,maximum,false};
    transport.failureResponder=[](const lmsc::AiTextRequest &message,const QJsonObject &) {
        lmsc::AiFailure failure; failure.category="truncated"; failure.maxOutputTokens=message.maxOutputTokens>0 ? message.maxOutputTokens : -1; return failure;
    };
    lmsc::LlmAiGenerationService service(&transport); QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_COMPARE(failed.count(),1); QCOMPARE(transport.requests.size(),1);
    service.cancel("job-1"); transport.policy={65536,131072,true}; transport.failureResponder={};
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady); service.resume("job-1"); QTRY_COMPARE(ready.count(),1);
    QCOMPARE(transport.requests[1].maxOutputTokens,65536);
}
void AiGenerationTest::nonTransientFailuresNeverRecover_data() {
    QTest::addColumn<QString>("category");
    QTest::addColumn<bool>("retryable");
    for (const QString category : {"authentication","permission","quota","parameters","tls","proxy","protocol"})
        QTest::newRow(qPrintable(category)) << category << true;
    QTest::newRow("unsupported-server") << QString("server") << true;
    QTest::newRow("network-not-retryable") << QString("network") << false;
}
void AiGenerationTest::nonTransientFailuresNeverRecover() {
    QFETCH(QString,category); QFETCH(bool,retryable); FakeTransport transport;
    transport.failureResponder=[category,retryable](const lmsc::AiTextRequest &,const QJsonObject &) {
        lmsc::AiFailure failure; failure.category=category; failure.retryable=retryable;
        if (category=="server") failure.httpStatus=501; return failure;
    };
    lmsc::LlmAiGenerationService service(&transport); QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed),ready(&service,&lmsc::AiGenerationService::draftReady);
    service.generate(request()); QTRY_COMPARE(failed.count(),1); QCOMPARE(transport.requests.size(),1);
    QCOMPARE(service.status().pauseCategory,category); QCOMPARE(ready.count(),0);
}
void AiGenerationTest::recoveryCanBeCancelledReconfiguredAndTimedOut_data() {
    QTest::addColumn<QString>("action");
    for (const QString action : {"cancel","discard","configure","timeout"}) QTest::newRow(qPrintable(action)) << action;
}
void AiGenerationTest::recoveryCanBeCancelledReconfiguredAndTimedOut() {
    QFETCH(QString,action); FakeTransport transport;
    transport.failureResponder=[](const lmsc::AiTextRequest &,const QJsonObject &) { lmsc::AiFailure failure; failure.category="network"; return failure; };
    lmsc::LlmAiGenerationService service(&transport); QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);
    service.generate(request()); QTRY_VERIFY(service.status().recovering);
    auto *timer=service.findChild<QTimer *>("aiGenerationRecoveryTimer"); QVERIFY(timer && timer->isActive()); QCOMPARE(timer->interval(),2000);
    auto *deadline=service.findChild<QTimer *>("aiGenerationTaskTimer"); QVERIFY(deadline && deadline->isActive());
    QVERIFY(deadline->remainingTime()<45*60000); const auto old=transport.requests.first();
    if (action=="cancel") service.cancel("job-1");
    else if (action=="discard") service.discard("job-1");
    else if (action=="configure") emit transport.configurationChanged();
    else { deadline->start(0); QTRY_COMPARE(service.status().state,lmsc::AiGenerationService::Status::Paused); }
    QVERIFY(!timer->isActive()); QVERIFY(!service.status().recovering);
    emit transport.completed({old.requestId,"{}",1,1}); QCOMPARE(ready.count(),0);
    QVERIFY(QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection)); QTest::qWait(30); QCOMPARE(transport.requests.size(),1);
}
void AiGenerationTest::bestCandidateSurvivesAutomaticRecovery() {
    FakeTransport transport; int calls=0; bool recovered=false,contextMatches=false; QString interrupted;
    transport.responder=[&](const lmsc::AiTextRequest &,const QJsonObject &input) {
        auto response=input.value("stage").toString()=="plan" ? validPlan(input) : fourNoteSegment(input);
        if (input.value("music").toObject().value("segmentId").toString()=="s2" && input.contains("motifReference")) {
            ++calls; auto rows=response.value("objects").toArray(); auto row=rows[0].toObject(); row["direction"]=0;
            if (calls>=3) row["anchorId"]="invalid"; rows[0]=row; response["objects"]=rows;
        }
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    };
    transport.failureResponder=[&](const lmsc::AiTextRequest &message,const QJsonObject &input) {
        lmsc::AiFailure failure;
        if (calls==2 && input.value("music").toObject().value("segmentId").toString()=="s2") {
            failure.category="network"; interrupted=message.userPrompt;
        } else if (calls==3) { recovered=true; contextMatches=message.userPrompt==interrupted; }
        return failure;
    };
    lmsc::LlmAiGenerationService service(&transport); accelerateRecovery(service);
    QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
    service.generate(request()); QTRY_COMPARE(ready.count(),1); QCOMPARE(failed.count(),0); QVERIFY(recovered); QVERIFY(contextMatches);
    const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first()); QVERIFY(draft.hasThemeWarnings);
    QCOMPARE(draft.objects[8].direction,0); QCOMPARE(draft.objects[0].direction,1);
}
void AiGenerationTest::manualContinueRaisesTruncatedBudget() {
    FakeTransport transport; transport.policy={8192,131072,false};
    transport.failureResponder=[](const lmsc::AiTextRequest &message,const QJsonObject &input) {
        lmsc::AiFailure failure;
        if (input.value("stage").toString()=="plan" && message.maxOutputTokens<65536) {
            failure.category="truncated"; failure.maxOutputTokens=message.maxOutputTokens;
        }
        return failure;
    };
    lmsc::LlmAiGenerationService service(&transport);
    QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed),ready(&service,&lmsc::AiGenerationService::draftReady);
    service.generate(request()); QTRY_COMPARE(failed.count(),1); QCOMPARE(transport.requests.size(),3);
    QCOMPARE(transport.requests.last().maxOutputTokens,32768);
    service.resume("job-1"); QTRY_COMPARE(ready.count(),1); QCOMPARE(failed.count(),1);
    QCOMPARE(transport.requests[3].maxOutputTokens,65536);
    QCOMPARE(transport.requests[2].userPrompt,transport.requests[3].userPrompt);
}
QTEST_GUILESS_MAIN(AiGenerationTest)
#include "AiGenerationTest.moc"
