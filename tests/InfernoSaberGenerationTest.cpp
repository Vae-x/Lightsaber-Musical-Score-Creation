#include "core/InfernoSaberGenerationService.h"
#include "core/ProjectStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QtEndian>
#include <cmath>
#include <cstdio>

namespace {
bool writeFile(const QString &path,const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(data)==data.size();
}
bool writeJson(const QString &path,const QJsonObject &value) {return writeFile(path,QJsonDocument(value).toJson());}
QJsonObject readJson(const QString &path) {QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(file.readAll()).object();}
QString arg(const QStringList &arguments,const QString &name) {const int index=arguments.indexOf(name);return index>=0&&index+1<arguments.size()?arguments[index+1]:QString{};}
QString hash(const QString &path) {QFile file(path);file.open(QIODevice::ReadOnly);return QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex();}
int fakeWorker(const QStringList &arguments) {
    QFile script(arguments.value(2));script.open(QIODevice::ReadOnly);const QString mode=QString::fromUtf8(script.readAll()).trimmed();
    const auto job=arg(arguments,"--job-dir"),audio=arg(arguments,"--audio"),difficulty=arg(arguments,"--difficulty");
    const auto reference=arg(arguments,"--rhythm-reference");const double bpm=arg(arguments,"--bpm").toDouble();
    if(!QDir().mkpath(job))return 4;
    const auto info=readJson(QDir(reference).filePath("info.dat"));
    if(info.value("_songTimeOffset").toDouble(-1)!=0||info.value("_beatsPerMinute").toDouble()!=bpm||
        hash(audio)!=hash(QDir(reference).filePath("song.ogg")))return 5;
    if(mode=="sleep") {
        std::puts("{\"stage\":\"generate\"}");std::fflush(stdout);
        QThread::msleep(15000);
    }
    if(mode=="failure") {
        writeJson(QDir(job).filePath("worker-result.json"),{{"complete",false},{"error","模拟模型失败"}});
        std::puts("模拟日志保留");return 9;
    }
    const auto output=QDir(job).filePath("prediction/new_map/song");QDir().mkpath(output);
    QFile::copy(audio,QDir(output).filePath("song.ogg"));
    QJsonArray notes;
    for(double seconds:{.25,2.0,2.0,4.0})
        notes.append(QJsonObject{{"_time",seconds*bpm/60},{"_lineIndex",1},{"_lineLayer",1},{"_type",0},{"_cutDirection",1}});
    notes.append(QJsonObject{{"_time",8.0},{"_lineIndex",3},{"_lineLayer",1},{"_type",3},{"_cutDirection",8}});
    if(mode=="protected") {auto note=notes.first().toObject();note.insert("_customData",QJsonObject{{"_track","protected"}});notes[0]=note;}
    const auto chart=difficulty+".dat";
    writeJson(QDir(output).filePath(chart),{{"_version","2.2.0"},{"_notes",notes},
        {"_obstacles",QJsonArray{QJsonObject{{"_time",4},{"_lineIndex",0},{"_type",0},{"_duration",1},{"_width",1}}}}, {"_events",QJsonArray{}}});
    writeJson(QDir(output).filePath("info.dat"),{{"_version","2.0.0"},{"_songName","模拟模型"},{"_songSubName",""},
        {"_songAuthorName","test"},{"_levelAuthorName","test"},{"_beatsPerMinute",bpm},{"_songTimeOffset",0},
        {"_songFilename","song.ogg"},{"_coverImageFilename",""},{"_difficultyBeatmapSets",QJsonArray{QJsonObject{
        {"_beatmapCharacteristicName","Standard"},{"_difficultyBeatmaps",QJsonArray{QJsonObject{{"_difficulty",difficulty},
        {"_difficultyRank",difficulty=="Hard"?5:7},{"_beatmapFilename",chart},{"_noteJumpMovementSpeed",12},{"_noteJumpStartBeatOffset",0}}}}}}}});
    writeJson(QDir(job).filePath("worker-result.json"),{{"complete",true},{"sourceAudioSha256",hash(audio)},
        {"audioTimeShiftSeconds",0},{"outputSongDirectory",output}});
    std::puts("{\"stage\":\"complete\"}");return 0;
}
}

class InfernoSaberGenerationTest final:public QObject {
    Q_OBJECT
private:
    QTemporaryDir temporary;
    QString pcm,audio;
    lmsc::InfernoSaberGenerationService::Config settings;
    lmsc::GenerationRequest request(const QString &id="model") const {
        lmsc::GenerationRequest value;value.jobId=id;value.documentId="document";value.documentRevision=7;
        value.profile=lmsc::DifficultyProfile::forName("Hard");value.allowedTypes=lmsc::DirectionalType|lmsc::DotType;
        value.audio.path=pcm;value.audio.sourcePath=audio;value.audio.durationSeconds=20;value.audio.sampleRate=2000;value.audio.channels=1;
        value.audio.lease=std::make_shared<int>(1);value.timeMap.configure(120,1);value.arrangementSeed=43;return value;
    }
    void mode(const QString &value) {QVERIFY(writeFile(settings.runnerPath,value.toUtf8()));}
    QStringList jobs() const {return QDir(settings.workDirectory).entryList(QDir::Dirs|QDir::NoDotAndDotDot);}
private slots:
    void initTestCase() {
        QVERIFY(temporary.isValid());pcm=temporary.filePath("music.pcm");audio=temporary.filePath("original.ogg");
        QVERIFY(writeFile(audio,"OggS-original-immutable"));
        QByteArray bytes;
        for(int frame=0;frame<40000;++frame) {
            const double time=frame/2000.0,age=std::fmod(time,.5);
            const double sample=(.025+(age<.06?.7*std::exp(-age*45):0))*std::sin(2*3.141592653589793*110*time);
            char raw[2];qToLittleEndian<qint16>(qint16(sample*30000),reinterpret_cast<uchar*>(raw));bytes.append(raw,2);
        }
        QVERIFY(writeFile(pcm,bytes));
        settings.runtimeDirectory=temporary.filePath("runtime");settings.modelCacheDirectory=temporary.filePath("models");
        settings.runnerPath=temporary.filePath("runner.txt");settings.workDirectory=temporary.filePath("jobs");settings.timeoutSeconds=30;
        QVERIFY(QDir().mkpath(QDir(settings.runtimeDirectory).filePath("env")));
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),QDir(settings.runtimeDirectory).filePath("env/python.exe")));
        QVERIFY(writeFile(QDir(settings.runtimeDirectory).filePath("source/main.py"),"fixture"));
        for(const auto &name:QStringList{"easy_15","expert_15"}) QVERIFY(writeFile(QDir(settings.modelCacheDirectory).filePath(name+"/.trial-model-manifest.json"),"{}"));
        mode("success");
    }
    void missingEnvironmentFailsWithoutTouchingAudio() {
        lmsc::InfernoSaberGenerationService service;auto missing=settings;missing.runtimeDirectory=temporary.filePath("missing");service.setConfig(missing);
        QVERIFY(!service.isAvailable());QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed);
        const auto before=hash(audio);service.generate(request());QCOMPARE(failed.size(),1);QCOMPARE(hash(audio),before);
        QVERIFY(failed.first().last().toString().contains("环境"));
    }
    void analysisOnlyWorksWithoutModelEnvironment() {
        lmsc::InfernoSaberGenerationService service;auto missing=settings;missing.runtimeDirectory=temporary.filePath("missing");service.setConfig(missing);
        QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady);auto value=request();value.analysisOnly=true;
        service.generate(value);QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,15000);
        const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first());QVERIFY(draft.objects.isEmpty());QVERIFY(!draft.summary.isEmpty());
    }
    void musicalReferenceUsesAbsoluteSecondsAndPreservesAudio() {
        mode("success");lmsc::InfernoSaberGenerationService service;service.setConfig(settings);QVERIFY(service.isAvailable());
        QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
        const auto before=hash(audio);service.generate(request());QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty()||!failed.isEmpty(),15000);
        QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first().last().toString()));QCOMPARE(ready.size(),1);
        const auto draft=qvariant_cast<lmsc::GenerationDraft>(ready.first().first());QCOMPARE(draft.objects.size(),2);
        QCOMPARE(draft.objects[0].beat,2.0);QCOMPARE(draft.objects[1].beat,6.0);QCOMPARE(hash(audio),before);
        QVERIFY(draft.requiresPlayabilityReview);QVERIFY(draft.playabilityActiveSeconds>0);
        QVERIFY(draft.warnings.join('\n').contains("去重 1"));QVERIFY(draft.warnings.join('\n').contains("零拍之前"));
        QCOMPARE(draft.metrics.bombs,0);QCOMPARE(draft.metrics.walls,0);QVERIFY(!draft.objects[0].id.isEmpty());
        for(const auto &object:draft.objects) QVERIFY(!object.isProtected());
        const auto directories=jobs();QVERIFY(!directories.isEmpty());
        const auto job=QDir(settings.workDirectory).filePath(directories.last());
        QCOMPARE(service.jobDirectory(draft.source.jobId),job);
        QVERIFY(service.jobDirectory("unrelated-job").isEmpty());
        const auto reference=readJson(QDir(job).filePath("rhythm-reference/info.dat"));QCOMPARE(reference.value("_songTimeOffset").toDouble(),0.0);
        QVERIFY(QFileInfo(QDir(job).filePath("validation.json")).isFile());
    }
    void failurePreservesLogsAndDoesNotEmitDraft() {
        mode("failure");lmsc::InfernoSaberGenerationService service;service.setConfig(settings);
        QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
        const auto before=hash(audio);service.generate(request());QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,15000);
        QCOMPARE(ready.size(),0);QCOMPARE(hash(audio),before);QVERIFY(failed.first().last().toString().contains("模拟模型失败"));
        QVERIFY(failed.first().last().toString().contains("任务目录"));
        QVERIFY(QFileInfo(QDir(service.jobDirectory(failed.first().first().toString())).filePath("model.log")).isFile());
    }
    void cancelRejectsLateResponseAndAllowsNextJob() {
        mode("sleep");lmsc::InfernoSaberGenerationService service;service.setConfig(settings);
        QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),cancelled(&service,&lmsc::AiGenerationService::cancelled);
        service.generate(request("cancel-me"));QTRY_VERIFY_WITH_TIMEOUT(service.status().percent>=50,15000);
        service.cancel("cancel-me");QCOMPARE(cancelled.size(),1);QCOMPARE(ready.size(),0);
        QVERIFY(!service.jobDirectory("cancel-me").isEmpty());
        mode("success");service.generate(request("next"));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,15000);
        QCOMPARE(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).source.jobId,QString("next"));
    }
    void timeoutStopsProcessAndPreservesNextJob() {
        mode("sleep");lmsc::InfernoSaberGenerationService service;auto limited=settings;limited.timeoutSeconds=1;service.setConfig(limited);
        QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),failed(&service,&lmsc::AiGenerationService::requestFailed);
        service.generate(request("timeout"));QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,10000);QCOMPARE(ready.size(),0);
        QVERIFY(failed.first().last().toString().contains("超过"));QCOMPARE(service.status().state,lmsc::AiGenerationService::Status::Failed);
        mode("success");service.setConfig(settings);service.generate(request("after-timeout"));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,15000);
        QCOMPARE(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).source.jobId,QString("after-timeout"));
    }
    void replacementRejectsOldProcessWithoutExplicitCancel() {
        mode("sleep");lmsc::InfernoSaberGenerationService service;service.setConfig(settings);
        QSignalSpy ready(&service,&lmsc::AiGenerationService::draftReady),cancelled(&service,&lmsc::AiGenerationService::cancelled);
        service.generate(request("old"));QTRY_VERIFY_WITH_TIMEOUT(service.status().percent>=50,15000);
        mode("success");service.generate(request("replacement"));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,15000);
        QCOMPARE(cancelled.size(),1);QCOMPARE(qvariant_cast<lmsc::GenerationDraft>(ready.first().first()).source.jobId,QString("replacement"));
    }
    void protectionCannotBeRemovedByNormalization() {
        mode("protected");lmsc::InfernoSaberGenerationService service;service.setConfig(settings);
        QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed),ready(&service,&lmsc::AiGenerationService::draftReady);
        service.generate(request());QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,15000);QCOMPARE(ready.size(),0);
    }
    void unsupportedDifficultyTypesAndTempoFailBeforeInference() {
        lmsc::InfernoSaberGenerationService service;service.setConfig(settings);QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed);
        auto value=request();value.profile=lmsc::DifficultyProfile::forName("Normal");service.generate(value);QCOMPARE(failed.size(),1);
        value=request();value.allowedTypes|=lmsc::BombType;service.generate(value);QCOMPARE(failed.size(),2);
        value=request();value.timeMap.configure(120,1,{{8,140}});service.generate(value);QCOMPARE(failed.size(),3);
    }
    void completionReleasesUnretainedSnapshotLease_data() {
        QTest::addColumn<bool>("analysisOnly");
        QTest::newRow("analysis-only")<<true;
        QTest::newRow("model-draft")<<false;
    }
    void completionReleasesUnretainedSnapshotLease() {
        QFETCH(bool,analysisOnly);mode("success");
        lmsc::InfernoSaberGenerationService service;service.setConfig(settings);
        int received=0;QString completedJob;bool validDuringSignal=false;
        connect(&service,&lmsc::AiGenerationService::draftReady,this,[&](const lmsc::GenerationDraft &draft) {
            ++received;completedJob=draft.source.jobId;validDuringSignal=bool(draft.source.audio.lease);
            // Keep only plain metadata: no consumer is retaining this snapshot.
        });
        QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed);
        auto value=request("lease-completed");value.analysisOnly=analysisOnly;
        std::weak_ptr<void> lease=value.audio.lease;
        service.generate(value);value.audio.lease.reset();
        QTRY_VERIFY_WITH_TIMEOUT(received||!failed.isEmpty(),15000);
        QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first().last().toString()));
        QCOMPARE(received,1);QVERIFY(validDuringSignal);QCOMPARE(completedJob,QString("lease-completed"));
        QCOMPARE(service.status().state,lmsc::AiGenerationService::Status::Completed);
        QCOMPARE(service.status().jobId,QString("lease-completed"));
        // The finished thread still owns its captured snapshot until deleteLater.
        QTRY_VERIFY_WITH_TIMEOUT(lease.expired(),5000);
    }
    void cancelledAnalysisReleasesLeaseAfterThreadStops() {
        lmsc::InfernoSaberGenerationService service;service.setConfig(settings);
        QSignalSpy cancelled(&service,&lmsc::AiGenerationService::cancelled),ready(&service,&lmsc::AiGenerationService::draftReady);
        auto value=request("lease-cancelled");value.analysisOnly=true;
        std::weak_ptr<void> lease=value.audio.lease;
        service.generate(value);value.audio.lease.reset();
        service.cancel("lease-cancelled");
        QCOMPARE(cancelled.size(),1);QCOMPARE(cancelled.first().first().toString(),QString("lease-cancelled"));
        QTRY_VERIFY_WITH_TIMEOUT(lease.expired(),15000);
        QCOMPARE(ready.size(),0);QCOMPARE(service.status().state,lmsc::AiGenerationService::Status::Idle);
    }
    void failedProcessReleasesLeaseWithoutLosingTaskId() {
        mode("failure");lmsc::InfernoSaberGenerationService service;service.setConfig(settings);
        QSignalSpy failed(&service,&lmsc::AiGenerationService::requestFailed),ready(&service,&lmsc::AiGenerationService::draftReady);
        auto value=request("lease-failed");std::weak_ptr<void> lease=value.audio.lease;
        service.generate(value);value.audio.lease.reset();
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,15000);
        QCOMPARE(failed.first().first().toString(),QString("lease-failed"));
        QCOMPARE(service.status().jobId,QString("lease-failed"));
        QCOMPARE(service.status().state,lmsc::AiGenerationService::Status::Failed);
        QTRY_VERIFY_WITH_TIMEOUT(lease.expired(),5000);QCOMPARE(ready.size(),0);
    }
};

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if(app.arguments().contains("worker"))return fakeWorker(app.arguments());
    InfernoSaberGenerationTest test;return QTest::qExec(&test,argc,argv);
}
#include "InfernoSaberGenerationTest.moc"
