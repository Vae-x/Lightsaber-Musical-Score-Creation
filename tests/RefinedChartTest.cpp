#include "core/BeatmapDocument.h"
#include "core/RefinementTypes.h"
#include "core/ProjectStore.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

using namespace lmsc;
namespace {
QString audio(QTemporaryDir &root) {
    const auto path = root.filePath("source.ogg");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("OggS synthetic refinement fixture") < 0) return {};
    return path;
}
BeatObject note(double beat, int x, int hand = 0) {
    BeatObject object; object.beat = beat; object.x = x; object.y = 1;
    object.color = hand; object.direction = 1; return object;
}
QString signature(const BeatmapDocument &doc) { return refinementBaselineHash(doc.objects()); }
}
class RefinedChartTest : public QObject {
    Q_OBJECT
private slots:
    void mixedPatchIsOneUndoAndKeepsIdentity();
    void invalidPatchesAreAtomic();
    void staleRevisionAndWrongDifficultyReject();
    void obstaclesAndCustomDataCannotBeChanged();
    void existingProjectCanBeRefinedAndReopened();
    void unknownSourceFieldsSurviveRefinementExport();
    void importedSongCannotUseRefinementApplication();
    void unchangedPatchDoesNotCreateUndo();
};
void RefinedChartTest::mixedPatchIsOneUndoAndKeepsIdentity() {
    QTemporaryDir root; BeatmapDocument doc; QString error;
    QVERIFY(doc.createNew(audio(root), "精修", 120, 0, {}, &error));
    QVERIFY(doc.applyGeneratedChart({note(2,1),note(4,2,1),note(6,0)},"Expert",7,doc.revision(),&error));
    const auto before = doc.objects(); const auto beforeHash = signature(doc);
    auto changed = before[0]; changed.y = 0; changed.direction = 6;
    RefinementPatch patch; patch.updates={changed}; patch.removals=QStringList{before[1].id}; patch.additions={note(8,3,1)};
    QVERIFY2(doc.applyRefinementPatch(patch,doc.currentDifficultyId(),doc.revision(),&error),qPrintable(error));
    QCOMPARE(doc.objects().size(),3);
    QSet<QString> ids; for(const auto &o:doc.objects()) ids.insert(o.id);
    QVERIFY(ids.contains(before[0].id)); QVERIFY(ids.contains(before[2].id)); QVERIFY(!ids.contains(before[1].id));
    const auto afterHash=signature(doc);
    QVERIFY(afterHash!=beforeHash); QVERIFY(doc.undo()); QCOMPARE(signature(doc),beforeHash);
    QVERIFY(doc.redo()); QCOMPARE(signature(doc),afterHash);
}
void RefinedChartTest::invalidPatchesAreAtomic() {
    QTemporaryDir root; BeatmapDocument doc; QString error;
    QVERIFY(doc.createNew(audio(root),"原子检查",120,0,{},&error));
    QVERIFY(doc.applyGeneratedChart({note(2,1),note(4,2,1)},"Expert",7,doc.revision(),&error));
    const auto before=doc.objects(); const auto hash=signature(doc); const auto revision=doc.revision();
    auto moved=before[0]; moved.y=0;
    QVector<RefinementPatch> cases;
    RefinementPatch duplicate; duplicate.updates={moved,moved}; cases.append(duplicate);
    RefinementPatch overlap; overlap.updates={moved}; overlap.removals=QStringList{moved.id}; cases.append(overlap);
    RefinementPatch missing; missing.updates={moved}; missing.removals=QStringList{"missing"}; cases.append(missing);
    RefinementPatch collision; collision.updates={moved}; collision.additions={note(4,2,1)}; cases.append(collision);
    RefinementPatch reused; auto fake=note(8,1); fake.id=before[1].id; reused.additions={fake}; cases.append(reused);
    RefinementPatch invalid; invalid.updates={moved}; auto outside=note(8,4); invalid.additions={outside}; cases.append(invalid);
    for(const auto &patch:cases) {
        QVERIFY(!doc.applyRefinementPatch(patch,doc.currentDifficultyId(),revision,&error));
        QVERIFY(!error.isEmpty()); QCOMPARE(signature(doc),hash); QCOMPARE(doc.revision(),revision);
    }
}
void RefinedChartTest::staleRevisionAndWrongDifficultyReject() {
    QTemporaryDir root; BeatmapDocument doc; QString error;
    QVERIFY(doc.createNew(audio(root),"修订",120,0,{},&error));
    QVERIFY(doc.addObject(note(2,1),&error)); const auto revision=doc.revision();
    auto moved=doc.objects()[0]; moved.y=0; RefinementPatch patch; patch.updates={moved};
    QVERIFY(doc.addObject(note(6,2,1),&error)); const auto hash=signature(doc);
    QVERIFY(!doc.applyRefinementPatch(patch,doc.currentDifficultyId(),revision,&error));
    QVERIFY(!doc.applyRefinementPatch(patch,"another-chart",doc.revision(),&error)); QCOMPARE(signature(doc),hash);
}
void RefinedChartTest::obstaclesAndCustomDataCannotBeChanged() {
    QTemporaryDir root; BeatmapDocument doc; QString error;
    QVERIFY(doc.createNew(audio(root),"颜色与障碍",120,0,{},&error));
    auto colored=note(2,1);
    colored.preservedCustomData={{"_customData",QJsonObject{{"_color",QJsonArray{.2,.3,.4,1}}}}};
    auto bomb=note(4,0); bomb.kind=ObjectKind::Bomb;
    auto wall=note(8,3); wall.kind=ObjectKind::Wall; wall.y=0; wall.height=5;
    QVERIFY(doc.applyGeneratedChart({colored,bomb,wall},"Expert",7,doc.revision(),&error));
    const auto originals=doc.objects(); const auto hash=signature(doc); const auto revision=doc.revision();
    RefinementPatch patch; auto edited=originals[0]; edited.y=0; patch.updates={edited};
    QVERIFY(doc.applyRefinementPatch(patch,doc.currentDifficultyId(),revision,&error));
    QCOMPARE(doc.objects()[0].preservedCustomData,originals[0].preservedCustomData);
    QVERIFY(doc.undo()); QCOMPARE(signature(doc),hash);
    auto bad=originals[0]; bad.preservedCustomData={}; patch.updates={bad};
    QVERIFY(!doc.applyRefinementPatch(patch,doc.currentDifficultyId(),doc.revision(),&error));
    patch.updates={originals[1]}; QVERIFY(!doc.applyRefinementPatch(patch,doc.currentDifficultyId(),doc.revision(),&error));
    patch.updates.clear(); patch.removals=QStringList{originals[2].id};
    QVERIFY(!doc.applyRefinementPatch(patch,doc.currentDifficultyId(),doc.revision(),&error)); QCOMPARE(signature(doc),hash);
}
void RefinedChartTest::existingProjectCanBeRefinedAndReopened() {
    QTemporaryDir root; BeatmapDocument doc; QString error;
    QVERIFY(doc.createNew(audio(root),"已有工程",120,.25,{},&error));
    QVERIFY(doc.applyGeneratedChart({note(2,1),note(4,2,1)},"Expert",7,doc.revision(),&error));
    const auto expert=doc.currentDifficultyId();
    QVERIFY(doc.applyGeneratedChart({note(8,0)},"Easy",1,doc.revision(),&error));
    const auto easy=doc.currentDifficultyId(); const auto easyHash=signature(doc);
    QVERIFY(doc.setDifficulty(expert,&error));
    const auto project=root.filePath("saved/project.lmsc"); QVERIFY(doc.saveProject(project,&error));
    QJsonObject manifest; QVERIFY(ProjectStore::readJson(project,&manifest,&error));
    const auto assets=QDir(QFileInfo(project).absolutePath()).filePath(manifest.value("assets").toString());
    QMap<QString,QByteArray> originalFiles;
    for(const auto &relative:ProjectStore::files(assets)) { QFile f(QDir(assets).filePath(relative)); QVERIFY(f.open(QIODevice::ReadOnly)); originalFiles.insert(relative,f.readAll()); }
    BeatmapDocument reopened; QVERIFY2(reopened.loadProject(project,&error),qPrintable(error)); QVERIFY(reopened.isNewSong());
    auto updated=reopened.objects()[0]; updated.y=0; RefinementPatch patch; patch.updates={updated}; patch.additions={note(6,3,1)};
    QVERIFY(reopened.applyRefinementPatch(patch,expert,reopened.revision(),&error));
    const auto refined=signature(reopened); QVERIFY(reopened.saveProject(project,&error));
    BeatmapDocument again; QVERIFY2(again.loadProject(project,&error),qPrintable(error)); QCOMPARE(signature(again),refined);
    QVERIFY(again.setDifficulty(easy,&error)); QCOMPARE(signature(again),easyHash);
    for(auto it=originalFiles.cbegin();it!=originalFiles.cend();++it) { QFile f(QDir(assets).filePath(it.key())); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(),it.value()); }
}
void RefinedChartTest::importedSongCannotUseRefinementApplication() {
    QTemporaryDir root; BeatmapDocument source; QString error;
    QVERIFY(source.createNew(audio(root),"导入边界",120,0,{},&error)); QVERIFY(source.addObject(note(2,1),&error));
    const auto folder=root.filePath("export"); QVERIFY(source.exportSong(folder,&error));
    BeatmapDocument imported; QVERIFY(imported.loadSong(folder,&error)); const auto hash=signature(imported);
    auto moved=imported.objects()[0]; moved.y=0; RefinementPatch patch; patch.updates={moved};
    QVERIFY(!imported.applyRefinementPatch(patch,imported.currentDifficultyId(),imported.revision(),&error)); QCOMPARE(signature(imported),hash);
}
void RefinedChartTest::unknownSourceFieldsSurviveRefinementExport() {
    QTemporaryDir root; BeatmapDocument doc; QString error;
    QVERIFY(doc.createNew(audio(root),"未知字段",120,0,{},&error)); QVERIFY(doc.addObject(note(2,1),&error));
    const auto filename=doc.difficulties().first().filename;
    const auto project=root.filePath("source/project.lmsc"); QVERIFY(doc.saveProject(project,&error));
    QJsonObject manifest; QVERIFY(ProjectStore::readJson(project,&manifest,&error));
    const auto assets=QDir(QFileInfo(project).absolutePath()).filePath(manifest.value("assets").toString());
    const auto chartPath=QDir(assets).filePath(filename);
    const auto infoPath=QDir(assets).filePath("Info.dat");
    QJsonObject chart,info;
    QVERIFY(ProjectStore::readJson(chartPath,&chart,&error)); QVERIFY(ProjectStore::readJson(infoPath,&info,&error));
    const QJsonObject opaque{{"array",QJsonArray{1,"opaque",false}},{"nested",QJsonObject{{"key","keep"}}}};
    chart.insert("futureChartPayload",opaque); info.insert("futureInfoPayload",opaque);
    QVERIFY(ProjectStore::writeJson(chartPath,chart,&error)); QVERIFY(ProjectStore::writeJson(infoPath,info,&error));
    // Construct a coherent synthetic project before loading it: raw resource
    // hashes and the working Info snapshot must describe this authored fixture.
    auto hashes=manifest.value("assetHashes").toObject();
    for(const auto &relative:ProjectStore::files(assets)) {
        QFile file(QDir(assets).filePath(relative)); QVERIFY(file.open(QIODevice::ReadOnly));
        hashes.insert(relative,QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex()));
    }
    manifest.insert("assetHashes",hashes);
    auto workingInfo=manifest.value("info").toObject(); workingInfo.insert("futureInfoPayload",opaque); manifest.insert("info",workingInfo);
    QVERIFY(ProjectStore::writeJson(project,manifest,&error));
    BeatmapDocument reopened; QVERIFY2(reopened.loadProject(project,&error),qPrintable(error));
    auto moved=reopened.objects().first(); moved.y=0; RefinementPatch patch; patch.updates={moved};
    QVERIFY(reopened.applyRefinementPatch(patch,reopened.currentDifficultyId(),reopened.revision(),&error));
    const auto output=root.filePath("output"); QVERIFY(reopened.exportSong(output,&error));
    QJsonObject exportedInfo,exportedChart; QVERIFY(ProjectStore::readJson(QDir(output).filePath("Info.dat"),&exportedInfo,&error));
    QVERIFY(ProjectStore::readJson(QDir(output).filePath("Expert.dat"),&exportedChart,&error));
    QCOMPARE(exportedInfo.value("futureInfoPayload"),QJsonValue(opaque)); QCOMPARE(exportedChart.value("futureChartPayload"),QJsonValue(opaque));
    QCOMPARE(exportedChart.value("_notes").toArray().first().toObject().value("_lineLayer").toInt(),0);
    QJsonObject unchanged; QVERIFY(ProjectStore::readJson(chartPath,&unchanged,&error)); QCOMPARE(unchanged,chart);
}
void RefinedChartTest::unchangedPatchDoesNotCreateUndo() {
    QTemporaryDir root; BeatmapDocument doc; QString error;
    QVERIFY(doc.createNew(audio(root),"空补丁",120,0,{},&error)); QVERIFY(!doc.canUndo());
    const auto revision=doc.revision(); QVERIFY(doc.applyRefinementPatch({},doc.currentDifficultyId(),revision,&error));
    QCOMPARE(doc.revision(),revision); QVERIFY(!doc.canUndo());
}
QTEST_GUILESS_MAIN(RefinedChartTest)
#include "RefinedChartTest.moc"
