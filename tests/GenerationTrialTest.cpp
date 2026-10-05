#include "core/BeatmapDocument.h"
#include "core/ProjectStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace lmsc;
namespace {
QJsonObject json(const QString &path) { QJsonObject value; QString error; if(!ProjectStore::readJson(path,&value,&error)) qFatal("%s",error.toUtf8().constData()); return value; }
void write(const QString &path,const QJsonObject &value) { QString error; if(!ProjectStore::writeJson(path,value,&error)) qFatal("%s",error.toUtf8().constData()); }
QByteArray hash(const QString &path) { QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {}; QCryptographicHash digest(QCryptographicHash::Sha256); digest.addData(&file); return digest.result(); }
QJsonObject note(double beat,int hand,int x,int direction) {
    return {{"_time",beat},{"_type",hand},{"_lineIndex",x},{"_lineLayer",1},{"_cutDirection",direction},{"trialUnknown","preserved"}};
}
}
class GenerationTrialTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void describeIsReadOnlyAndKeepsTiming();
    void checkKeepsActionsAndFiltersHazards();
    void checkKeepsLegacyTimingProtection();
    void unsafeModelRemainsEditableAndFailsBackstroke();
    void repeatedPatternIsReported();
    void rejectsExistingOutputAndInvalidNotes();
    void rejectsReportsInsideSource();
    void localIsDeterministicAndPreservesAudio();
private:
    int invoke(const QStringList &arguments);
    QString rawSong(const QString &name,const QJsonArray &notes);
    QString path(const QString &name) const { return QDir(m_temp.path()).filePath(name); }
    QTemporaryDir m_temp; QString m_audio,m_project;
};
int GenerationTrialTest::invoke(const QStringList &arguments) {
    QProcess process; process.start(QString::fromUtf8(LMSC_TRIAL_EXECUTABLE),arguments);
    if(!process.waitForStarted(10000)) { qWarning()<<process.errorString(); return -1; }
    if(!process.waitForFinished(120000)) { process.kill(); process.waitForFinished(); return -2; }
    if(process.exitStatus()!=QProcess::NormalExit) { qWarning()<<process.readAllStandardError(); return -3; }
    if(process.exitCode()!=0) qWarning().noquote()<<QString::fromUtf8(process.readAllStandardError());
    return process.exitCode();
}
void GenerationTrialTest::initTestCase() {
    QVERIFY(m_temp.isValid()); m_audio=path(QStringLiteral("中文空格 music.ogg"));
    QProcess ffmpeg; ffmpeg.start(QDir(QString::fromUtf8(LMSC_TRIAL_TOOLS)).filePath("ffmpeg.exe"),
        {"-hide_banner","-v","error","-nostdin","-n","-f","lavfi","-i",
         "aevalsrc=0.025*sin(2*PI*110*t)+if(lt(mod(t\\,0.5)\\,0.06)\\,0.65*sin(2*PI*110*t)*exp(-45*mod(t\\,0.5))\\,0):s=44100",
         "-t","16","-ac","2","-c:a","libvorbis",m_audio});
    QVERIFY(ffmpeg.waitForStarted(10000)); QVERIFY(ffmpeg.waitForFinished(30000));
    QVERIFY2(ffmpeg.exitCode()==0,ffmpeg.readAllStandardError().constData());
    BeatmapDocument document; QString error;
    QVERIFY2(document.createNew(m_audio,QStringLiteral("合成中文歌曲"),120,.25,{},&error),qPrintable(error));
    m_project=path(QStringLiteral("原始工程 中文/project.lmsc"));
    QVERIFY2(document.saveProject(m_project,&error),qPrintable(error));
}
QString GenerationTrialTest::rawSong(const QString &name,const QJsonArray &notes) {
    const auto folder=path(name); QDir().mkpath(folder); QFile::copy(m_audio,QDir(folder).filePath("music.egg"));
    QJsonObject descriptor{{"_difficulty","ExpertPlus"},{"_difficultyRank",9},{"_beatmapFilename","ExpertPlus.dat"},
        {"_noteJumpMovementSpeed",12},{"_noteJumpStartBeatOffset",0}};
    write(QDir(folder).filePath("Info.dat"),{{"_version","2.0.0"},{"_songName",QStringLiteral("现成模型合成谱")},
        {"_songAuthorName","fixture"},{"_levelAuthorName","fixture"},{"_beatsPerMinute",120},{"_songTimeOffset",0},
        {"_shuffle",0},{"_shufflePeriod",.5},{"_songFilename","music.egg"},{"_coverImageFilename",""},
        {"_environmentName","DefaultEnvironment"},
        {"_difficultyBeatmapSets",QJsonArray{QJsonObject{{"_beatmapCharacteristicName","Standard"},{"_difficultyBeatmaps",QJsonArray{descriptor}}}}}});
    write(QDir(folder).filePath("ExpertPlus.dat"),{{"_version","2.0.0"},{"_notes",notes},
        {"_obstacles",QJsonArray{QJsonObject{{"_time",7},{"_lineIndex",0},{"_type",0},{"_duration",1},{"_width",1}}}},
        {"_events",QJsonArray{}},{"_waypoints",QJsonArray{}},{"trialMapUnknown","preserved"}});
    return folder;
}
void GenerationTrialTest::describeIsReadOnlyAndKeepsTiming() {
    const auto before=hash(m_project); const auto report=path("describe.json");
    QCOMPARE(invoke({"describe","--project",m_project,"--report",report}),0);
    const auto result=json(report); QCOMPARE(result.value("songName").toString(),QStringLiteral("合成中文歌曲"));
    QCOMPARE(result.value("bpm").toDouble(),120.0); QCOMPARE(result.value("firstBeatSeconds").toDouble(),.25);
    QVERIFY(result.value("changes").toArray().isEmpty()); QCOMPARE(hash(m_project),before);
    QVERIFY(QFileInfo::exists(result.value("audioPath").toString()));
    QCOMPARE(hash(result.value("audioPath").toString()),hash(m_audio));
    QCOMPARE(result.value("sourceHashes").toObject().value("audioSha256").toString(),QString::fromLatin1(hash(m_audio).toHex()));
}
void GenerationTrialTest::checkKeepsActionsAndFiltersHazards() {
    QJsonArray notes{note(1,0,1,1),note(1,1,2,1),note(2,0,1,0),note(2,1,2,0),note(5,3,0,8)};
    const auto source=rawSong("raw valid 中文",notes), output=path("checked valid 中文"), report=path("checked.json");
    const auto mapBefore=hash(QDir(source).filePath("ExpertPlus.dat"));
    QCOMPARE(invoke({"check","--song",source,"--difficulty","Hard","--output",output,"--report",report,"--tools",QString::fromUtf8(LMSC_TRIAL_TOOLS)}),0);
    const auto result=json(report); QVERIFY(result.value("formatValid").toBool()); QVERIFY(result.value("playabilityPass").toBool()); QVERIFY(result.value("playableReady").toBool());
    QCOMPARE(result.value("removedBombs").toInt(),1); QCOMPARE(result.value("removedWalls").toInt(),1);
    const auto exported=json(QDir(output).filePath("Hard.dat")); notes.removeLast(); QCOMPARE(exported.value("_notes").toArray(),notes);
    QCOMPARE(exported.value("_version").toString(),QStringLiteral("2.2.0"));
    QCOMPARE(json(QDir(source).filePath("ExpertPlus.dat")).value("_version").toString(),QStringLiteral("2.0.0"));
    QCOMPARE(result.value("sourceChartVersion").toString(),QStringLiteral("2.0.0"));
    QCOMPARE(result.value("outputChartVersion").toString(),QStringLiteral("2.2.0"));
    QVERIFY(exported.value("_obstacles").toArray().isEmpty()); QCOMPARE(exported.value("trialMapUnknown").toString(),QStringLiteral("preserved"));
    const auto descriptor=json(QDir(output).filePath("Info.dat")).value("_difficultyBeatmapSets").toArray().first().toObject().value("_difficultyBeatmaps").toArray().first().toObject();
    QCOMPARE(descriptor.value("_difficulty").toString(),QStringLiteral("Hard")); QCOMPARE(descriptor.value("_difficultyRank").toInt(),5);
    QCOMPARE(hash(QDir(output).filePath("song.ogg")),hash(m_audio)); QCOMPARE(hash(QDir(source).filePath("ExpertPlus.dat")),mapBefore);
}
void GenerationTrialTest::checkKeepsLegacyTimingProtection() {
    const auto source=rawSong("raw protected legacy timing",QJsonArray{note(1,0,1,1)});
    const auto mapPath=QDir(source).filePath("ExpertPlus.dat"); auto map=json(mapPath);
    map.insert("_events",QJsonArray{QJsonObject{{"_time",2},{"_type",10},{"_value",180}}}); write(mapPath,map);
    const auto before=hash(mapPath); const auto output=path("protected legacy timing output"),report=path("protected-legacy.json");
    QCOMPARE(invoke({"check","--song",source,"--difficulty","Hard","--output",output,"--report",report,"--tools",QString::fromUtf8(LMSC_TRIAL_TOOLS)}),2);
    QCOMPARE(hash(mapPath),before); QVERIFY(!QFileInfo::exists(output));
    QVERIFY(!json(report).value("formatValid").toBool()); QVERIFY(!json(report).value("playableReady").toBool());
}
void GenerationTrialTest::unsafeModelRemainsEditableAndFailsBackstroke() {
    const QJsonArray notes{note(1,0,1,1),note(2,0,1,1)};
    const auto source=rawSong("raw unsafe",notes),output=path("editor only"),report=path("unsafe.json");
    QCOMPARE(invoke({"check","--song",source,"--difficulty","Expert","--output",output,"--report",report,"--tools",QString::fromUtf8(LMSC_TRIAL_TOOLS)}),0);
    const auto result=json(report); QVERIFY(result.value("formatValid").toBool()); QVERIFY(!result.value("playabilityPass").toBool()); QVERIFY(!result.value("valid").toBool());
    QCOMPARE(result.value("metrics").toObject().value("backstrokeViolations").toInt(),1); QVERIFY(!result.value("errors").toArray().isEmpty());
    QCOMPARE(json(QDir(output).filePath("Expert.dat")).value("_notes").toArray(),notes);
    QCOMPARE(json(QDir(output).filePath("Expert.dat")).value("_version").toString(),QStringLiteral("2.2.0"));
    BeatmapDocument document; QString error; QVERIFY2(document.loadSong(output,&error),qPrintable(error));
    QVERIFY(document.readOnlyReason().isEmpty()); QCOMPARE(document.objects().size(),2);
}
void GenerationTrialTest::repeatedPatternIsReported() {
    QJsonArray notes;
    for(int i=0;i<48;++i) notes.append(note(.5*i, i%2, i%2?2:1, (i/2)%2?0:1));
    const auto source=rawSong("raw repeated",notes),output=path("repeated"),report=path("repeat.json");
    QCOMPARE(invoke({"check","--song",source,"--difficulty","Expert","--output",output,"--report",report,"--tools",QString::fromUtf8(LMSC_TRIAL_TOOLS)}),0);
    const auto metrics=json(report).value("metrics").toObject(); QVERIFY(metrics.value("fixed16LoopOccurrences").toInt()>0);
    QVERIFY(metrics.value("longestRepeatedActionRun").toInt()>=32); QCOMPARE(metrics.value("directionCounts").toArray().size(),9);
    QCOMPARE(metrics.value("gridCounts").toArray().size(),12);
}
void GenerationTrialTest::rejectsExistingOutputAndInvalidNotes() {
    const auto source=rawSong("raw reject",QJsonArray{note(1,0,1,1)}),output=path("already there"); QDir().mkpath(output);
    const auto sentinel=QDir(output).filePath("sentinel.txt"); QFile file(sentinel); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("keep"); file.close();
    QCOMPARE(invoke({"check","--song",source,"--difficulty","Hard","--output",output,"--report",path("reject-existing.json"),"--tools",QString::fromUtf8(LMSC_TRIAL_TOOLS)}),2);
    QCOMPARE(hash(sentinel),QCryptographicHash::hash("keep",QCryptographicHash::Sha256));
    auto bad=note(1,0,1,1); bad.insert("_lineIndex",1.2);
    const auto badSource=rawSong("raw invalid",QJsonArray{bad}),badOutput=path("invalid output"),badReport=path("invalid.json");
    QCOMPARE(invoke({"check","--song",badSource,"--difficulty","Hard","--output",badOutput,"--report",badReport,"--tools",QString::fromUtf8(LMSC_TRIAL_TOOLS)}),2);
    QVERIFY(!QFileInfo::exists(badOutput)); QVERIFY(!json(badReport).value("formatValid").toBool());
    const auto first=path("describe.json"); const auto before=hash(first);
    QCOMPARE(invoke({"describe","--project",m_project,"--report",first}),2); QCOMPARE(hash(first),before);
}
void GenerationTrialTest::rejectsReportsInsideSource() {
    const auto folder=QFileInfo(m_project).absolutePath(); const auto filesBefore=ProjectStore::files(folder);
    const auto projectHash=hash(m_project); const auto insideReport=QDir(folder).filePath("new-report.json");
    QCOMPARE(invoke({"describe","--project",m_project,"--report",insideReport}),2);
    QVERIFY(!QFileInfo::exists(insideReport)); QCOMPARE(ProjectStore::files(folder),filesBefore); QCOMPARE(hash(m_project),projectHash);
    const auto source=rawSong("raw report guard",QJsonArray{note(1,0,1,1)}); const auto rawFiles=ProjectStore::files(source);
    const auto rawHash=hash(QDir(source).filePath("ExpertPlus.dat")); const auto rawReport=QDir(source).filePath("new/sub/report.json");
    const auto output=path("report guard output");
    QCOMPARE(invoke({"check","--song",source,"--difficulty","Hard","--output",output,"--report",rawReport,"--tools",QString::fromUtf8(LMSC_TRIAL_TOOLS)}),2);
    QVERIFY(!QFileInfo::exists(rawReport)); QVERIFY(!QFileInfo::exists(output));
    QCOMPARE(ProjectStore::files(source),rawFiles); QCOMPARE(hash(QDir(source).filePath("ExpertPlus.dat")),rawHash);
}
void GenerationTrialTest::localIsDeterministicAndPreservesAudio() {
    const auto before=hash(m_project);
    for(int iteration=0;iteration<2;++iteration) {
        const auto output=path("local"+QString::number(iteration)),report=path("local"+QString::number(iteration)+".json");
        QCOMPARE(invoke({"local","--project",m_project,"--difficulty","Hard","--seed","20261005","--output",output,"--report",report,"--tools",QString::fromUtf8(LMSC_TRIAL_TOOLS)}),0);
        const auto result=json(report); QVERIFY(result.value("playableReady").toBool());
        QCOMPARE(result.value("metrics").toObject().value("backstrokeViolations").toInt(),0);
        QCOMPARE(hash(QDir(output).filePath("song.ogg")),hash(m_audio));
    }
    QCOMPARE(json(path("local0/Hard.dat")),json(path("local1/Hard.dat"))); QCOMPARE(hash(m_project),before);
    const auto map=json(path("local0/Hard.dat")); QVERIFY(!map.value("_notes").toArray().isEmpty());
    QVERIFY(map.value("_notes").toArray().first().toObject().value("_time").toDouble()>=.5);
}
QTEST_GUILESS_MAIN(GenerationTrialTest)
#include "GenerationTrialTest.moc"
