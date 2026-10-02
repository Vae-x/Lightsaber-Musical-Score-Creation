#include "core/AiGenerationService.h"
#include "core/AiTextTransport.h"
#include "core/BeatmapPlayabilityValidator.h"
#include "core/MusicFeatureAnalyzer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSignalSpy>
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

class FakeTransport final : public lmsc::AiTextTransport {
public:
    using AiTextTransport::AiTextTransport;
    bool available=true, hold=false, networkFailure=false;
    QVector<lmsc::AiTextRequest> requests;
    QStringList cancellations;
    std::function<QString(const lmsc::AiTextRequest &, const QJsonObject &)> responder;
    bool isAvailable() const override { return available; }
    void configure(const lmsc::AppPreferences &) override {}
    void complete(const lmsc::AiTextRequest &request) override {
        requests.append(request);
        if (hold) return;
        const auto input=QJsonDocument::fromJson(request.userPrompt.toUtf8()).object();
        const QString answer=responder ? responder(request,input)
            : QString::fromUtf8(QJsonDocument(input.value("stage").toString()=="plan" ? validPlan(input) : validSegment(input)).toJson(QJsonDocument::Compact));
        QTimer::singleShot(0,this,[this,request,answer] {
            if (networkFailure) emit failed(request.requestId,QStringLiteral("模拟网络失败"));
            else emit completed({request.requestId,answer,10,20});
        });
    }
    void cancel(const QString &id) override { cancellations.append(id); }
};
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
    void planJsonIsRepairedOnce();
    void segmentRepairIsBoundedWithoutFallback();
    void cancellationAndStaleResponses();
    void destroyedTransportAndServiceAreSafe();
    void requestAndWholeTaskTimeouts();
    void networkFailuresDoNotRetry();
    void emptyOrUnknownOptionsAreRejected();
    void completedAndCancelledTasksReleaseAudioLease();
private:
    QTemporaryDir temporary;
    QString shortPcm,longPcm;
    lmsc::GenerationRequest request(double duration=24.0) const;
};

lmsc::GenerationRequest AiGenerationTest::request(double duration) const {
    lmsc::GenerationRequest result;
    result.jobId="job-1"; result.documentId="new-song"; result.documentRevision=1; result.audioRevision=1;
    result.difficultyId="difficulty-1";
    result.profile=lmsc::DifficultyProfile::forName("Expert");
    result.audio.path=duration>30 ? longPcm : shortPcm;
    result.audio.sourcePath="test-only-local-song.ogg";
    result.audio.revision=1; result.audio.sampleRate=2000; result.audio.channels=2; result.audio.durationSeconds=duration;
    result.timeMap.configure(120,0);
    return result;
}
void AiGenerationTest::initTestCase() {
    QVERIFY(temporary.isValid());
    shortPcm=temporary.filePath("short.pcm"); longPcm=temporary.filePath("long.pcm");
    QVERIFY(makePcm(shortPcm,24)); QVERIFY(makePcm(longPcm,188));
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
        QCOMPARE(message.timeoutMs,180000); QVERIFY(!message.userPrompt.contains(shortPcm));
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
void AiGenerationTest::segmentRepairIsBoundedWithoutFallback() {
    FakeTransport transport;
    transport.responder=[](const lmsc::AiTextRequest &,const QJsonObject &input) {
        QJsonObject response=input.value("stage").toString()=="plan" ? validPlan(input) : validSegment(input);
        if (input.value("stage").toString()=="segment") {
            auto rows=response.value("objects").toArray(); auto row=rows[0].toObject(); row.insert("anchorId","hallucinated"); rows[0]=row; response.insert("objects",rows);
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
    auto *timer=service.findChild<QTimer *>("aiGenerationRequestTimer"); QVERIFY(timer); timer->start(1);
    QTRY_COMPARE(failed.count(),1); QVERIFY(!transport.cancellations.isEmpty());
    service.generate(request(188)); timer=service.findChild<QTimer *>("aiGenerationTaskTimer"); QVERIFY(timer); timer->start(1);
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
    ready.clear(); QTRY_VERIFY(weak.expired());
    source=request(188); lease=std::make_shared<int>(2); weak=lease;
    source.audio.lease=lease; lease.reset(); service.generate(source); source.audio.lease.reset(); service.cancel(source.jobId);
    QTRY_VERIFY(weak.expired());
}

QTEST_GUILESS_MAIN(AiGenerationTest)
#include "AiGenerationTest.moc"
