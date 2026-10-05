#include "core/BeatmapDocument.h"
#include "core/RefinementTypes.h"
#include "core/ProjectStore.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

using namespace lmsc;
namespace {
QString audio(QTemporaryDir &root) {
    const QString path = root.filePath("input.ogg");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("OggS independent draft fixture") < 0) return {};
    return path;
}
BeatObject note(double beat, int x, int hand = 0) {
    BeatObject value; value.beat = beat; value.x = x; value.y = 1; value.color = hand; value.direction = 1;
    return value;
}
BeatObject bomb(double beat, int x) {
    auto value = note(beat, x); value.kind = ObjectKind::Bomb; value.direction = 8; return value;
}
BeatObject wall(double beat, int x) {
    auto value = note(beat, x); value.kind = ObjectKind::Wall; value.y = 0;
    value.duration = 2; value.height = 5; return value;
}
EditingDraftApplication application(const BeatmapDocument &formal, const BeatmapDocument &snapshot) {
    EditingDraftApplication value; value.targetDifficultyId = snapshot.currentDifficultyId();
    value.baselineHash = snapshot.editingBaselineHash(); value.expectedRevision = formal.revision();
    value.objects = snapshot.objects(); return value;
}
QString signature(const BeatmapDocument &doc) { return refinementBaselineHash(doc.objects()); }
bool importFixture(QTemporaryDir &root, BeatmapDocument *document, QJsonObject *raw, QString *error) {
    BeatmapDocument seed;
    if (!seed.createNew(audio(root), "保留原字段", 120, 0, {}, error)
        || !seed.applyGeneratedChart({note(2,0),note(4,1),note(6,2,1),bomb(8,3),wall(10,0)},
            "Expert",7,seed.revision(),error)) return false;
    const QString folder = root.filePath("imported");
    if (!seed.exportSong(folder,error) || !ProjectStore::readJson(QDir(folder).filePath("Expert.dat"),raw,error)) return false;
    auto notes = raw->value("_notes").toArray();
    auto protectedNote = notes[0].toObject();
    protectedNote.insert("_customData",QJsonObject{{"_animation",QJsonObject{{"_dissolve",QJsonArray{1,0}}}}});
    notes[0] = protectedNote;
    auto colored = notes[1].toObject();
    colored.insert("_customData",QJsonObject{{"_color",QJsonArray{.1,.2,.3,1}}});
    colored.insert("futureObjectData",QJsonObject{{"opaque","preserve"}}); notes[1] = colored;
    raw->insert("_notes",notes); raw->insert("futureMapData",QJsonArray{"opaque",7,false});
    return ProjectStore::writeJson(QDir(folder).filePath("Expert.dat"),*raw,error) && document->loadSong(folder,error);
}
}

class EditingDraftTest final : public QObject {
    Q_OBJECT
private slots:
    void snapshotIsIndependentAndResourcesOutliveFormal();
    void snapshotCannotSaveOrChangeGlobalData();
    void selectingExistingAndNewTargetsDoesNotMutateFormal();
    void mixedManualDraftAppliesOnceAndPreservesOtherDifficulty();
    void protectedAndUnknownDataSurviveGeneralApplication();
    void invalidDraftsAreAtomic();
    void restoringSnapshotRevivesIdsAndClearsHistory();
    void newTargetApplicationHasOneUndo();
    void draftRecordsAreDirtyWithoutChangingFormalRevision();
    void legacyAndDamagedDraftRecordsRoundTrip();
};

void EditingDraftTest::snapshotIsIndependentAndResourcesOutliveFormal() {
    QTemporaryDir root; auto formal = std::unique_ptr<BeatmapDocument>(new BeatmapDocument); QString error;
    QVERIFY(formal->createNew(audio(root),"独立草稿",120,.25,{},&error));
    QVERIFY(formal->addObject(note(2,0),&error));
    const QString original = signature(*formal), path = formal->audioPath();
    const auto revision = formal->revision(); auto snapshot = formal->createEditingSnapshot(&error);
    QVERIFY2(snapshot,qPrintable(error)); QVERIFY(snapshot->isEditingSnapshot());
    QCOMPARE(snapshot->audioPath(),path); QCOMPARE(signature(*snapshot),original);
    QCOMPARE(snapshot->objects().first().id,formal->objects().first().id);
    QVERIFY(!snapshot->canUndo()); QVERIFY(!snapshot->canRedo()); QVERIFY(!snapshot->isModified());
    auto moved = snapshot->objects().first(); moved.y = 2;
    QVERIFY(snapshot->updateObject(moved,&error)); QVERIFY(snapshot->canUndo());
    QCOMPARE(formal->revision(),revision); QCOMPARE(signature(*formal),original);
    QCOMPARE(snapshot->editingBaselineHash(),original);
    QVERIFY(snapshot->undo()); QCOMPARE(signature(*snapshot),original);
    QVERIFY(snapshot->redo()); QVERIFY(signature(*snapshot)!=original);
    // Reloading releases the formal lease; the draft still owns the old audio.
    QVERIFY(formal->createNew(audio(root),"另一个正式工程",100,0,{},&error));
    QVERIFY(QFileInfo::exists(path)); formal.reset(); QVERIFY(QFileInfo::exists(path));
    snapshot.reset(); QVERIFY(!QFileInfo::exists(path));
}

void EditingDraftTest::snapshotCannotSaveOrChangeGlobalData() {
    QTemporaryDir root; BeatmapDocument formal; QString error; const auto input = audio(root);
    QVERIFY(formal.createNew(input,"全局限制",120,0,{},&error));
    auto snapshot = formal.createEditingSnapshot(&error); QVERIFY(snapshot);
    const auto revision = snapshot->revision();
    QVERIFY(!snapshot->saveProject(root.filePath("must-not-exist/project.lmsc"),&error));
    QVERIFY(!snapshot->autoSave(&error)); QVERIFY(!snapshot->exportSong(root.filePath("must-not-export"),&error));
    QVERIFY(!snapshot->setNewSongTempo(150,.5,&error));
    QVERIFY(!snapshot->setNewSongMetadata("改名","作者","制谱",&error));
    QVERIFY(!snapshot->setNewSongDifficulty("Hard",5,&error));
    QVERIFY(!snapshot->setImportSource(input,0,0,10,&error));
    QVERIFY(!snapshot->createNew(input,"替换资源",130,0,{},&error));
    QVERIFY(!snapshot->loadSong(root.path(),&error)); QVERIFY(!snapshot->loadZip(root.filePath("none.zip"),&error));
    QVERIFY(!snapshot->loadProject(root.filePath("none.lmsc"),&error));
    QVERIFY(!snapshot->setEditorDraftRecords(QJsonArray{},&error));
    QVERIFY(!snapshot->applyGeneratedChart({note(2,0)},"Hard",5,revision,&error));
    QCOMPARE(snapshot->revision(),revision); QCOMPARE(snapshot->title(),QString("全局限制"));
    QCOMPARE(snapshot->timeMap().initialBpm(),120.0); QCOMPARE(snapshot->timeMap().offsetSeconds(),0.0);
    QVERIFY(!QFileInfo::exists(root.filePath("must-not-exist")));
    QVERIFY(!QFileInfo::exists(root.filePath("must-not-export")));
}

void EditingDraftTest::selectingExistingAndNewTargetsDoesNotMutateFormal() {
    QTemporaryDir root; BeatmapDocument formal; QString error;
    QVERIFY(formal.createNew(audio(root),"分难度草稿",120,0,{},&error));
    QVERIFY(formal.applyGeneratedChart({note(2,0)},"Expert",7,formal.revision(),&error));
    const QString expert = formal.currentDifficultyId();
    QVERIFY(formal.applyGeneratedChart({note(4,1)},"Easy",1,formal.revision(),&error));
    const QString easy = formal.currentDifficultyId(); const auto revision = formal.revision();
    auto existing = formal.createEditingSnapshotForDifficulty(expert,{},7,&error); QVERIFY(existing);
    QCOMPARE(existing->currentDifficultyId(),expert); QCOMPARE(existing->objects().first().beat,2.0);
    QCOMPARE(formal.currentDifficultyId(),easy); QCOMPARE(formal.revision(),revision);
    QVERIFY(!existing->setDifficulty(easy,&error));
    auto added = formal.createEditingSnapshotForDifficulty({},"Hard",5,&error); QVERIFY2(added,qPrintable(error));
    QCOMPARE(added->objects().size(),0); QCOMPARE(added->editingBaselineHash(),refinementBaselineHash({}));
    QCOMPARE(formal.difficulties().size(),2); QCOMPARE(formal.currentDifficultyId(),easy);
    QCOMPARE(formal.revision(),revision); QCOMPARE(formal.editingBaselineHash(expert),existing->editingBaselineHash());
    QVERIFY(!formal.createEditingSnapshotForDifficulty("missing",{},7,&error));
    QVERIFY(!formal.createEditingSnapshotForDifficulty({},"Hard",7,&error));
    QVERIFY(!formal.createEditingSnapshotForDifficulty({},"Easy",1,&error));
    QVERIFY(formal.applyEditingDraft(application(formal,*existing),&error));
    QCOMPARE(formal.currentDifficultyId(),expert); QCOMPARE(formal.revision(),revision+1);
}

void EditingDraftTest::mixedManualDraftAppliesOnceAndPreservesOtherDifficulty() {
    QTemporaryDir root; BeatmapDocument formal; QString error;
    QVERIFY(formal.createNew(audio(root),"三种物件",120,0,{},&error));
    QVERIFY(formal.applyGeneratedChart({note(2,0),bomb(4,1),wall(6,3),note(10,2,1)},"Expert",7,formal.revision(),&error));
    const QString expert = formal.currentDifficultyId(), before = signature(formal);
    QVERIFY(formal.applyGeneratedChart({note(2,1)},"Easy",1,formal.revision(),&error));
    const QString easy = formal.currentDifficultyId(), other = signature(formal);
    auto snapshot = formal.createEditingSnapshotForDifficulty(expert,{},7,&error); QVERIFY(snapshot);
    auto objects = snapshot->objects(); const auto originalId = objects[0].id;
    objects[0].direction = 6; objects[0].y = 0;
    objects[1].x = 0; objects[2].duration = 3; objects[2].width = 1;
    objects.removeLast(); auto extra = note(2.01,2); extra.id = "manual:extra"; objects.append(extra);
    // A manual fast sequence and deliberate wall placement remain legal here.
    QVERIFY2(snapshot->replaceEditingSnapshotObjects(objects,false,&error),qPrintable(error));
    const auto revision = formal.revision(); auto request = application(formal,*snapshot);
    QVERIFY2(formal.applyEditingDraft(request,&error),qPrintable(error));
    QCOMPARE(formal.revision(),revision+1); QCOMPARE(formal.currentDifficultyId(),expert);
    const QString after = signature(formal); QVERIFY(after!=before);
    QCOMPARE(formal.objects().first().id,originalId);
    QVERIFY(formal.undo()); QCOMPARE(signature(formal),before); QVERIFY(formal.redo()); QCOMPARE(signature(formal),after);
    QVERIFY(formal.setDifficulty(easy,&error)); QCOMPARE(signature(formal),other);
    const QString project = root.filePath("saved/project.lmsc"); QVERIFY(formal.saveProject(project,&error));
    BeatmapDocument reopened; QVERIFY2(reopened.loadProject(project,&error),qPrintable(error));
    QCOMPARE(signature(reopened),other); QVERIFY(reopened.setDifficulty(expert,&error)); QCOMPARE(signature(reopened),after);
}

void EditingDraftTest::protectedAndUnknownDataSurviveGeneralApplication() {
    QTemporaryDir root; BeatmapDocument formal; QString error; QJsonObject raw;
    QVERIFY2(importFixture(root,&formal,&raw,&error),qPrintable(error));
    const auto before = formal.objects(); QVERIFY(before[0].isProtected()); QVERIFY(!before[1].isProtected());
    auto snapshot = formal.createEditingSnapshot(&error); QVERIFY(snapshot);
    QCOMPARE(snapshot->objects()[0].protectedReason,before[0].protectedReason);
    QCOMPARE(snapshot->objects()[1].preservedCustomData,before[1].preservedCustomData);
    auto changed = snapshot->objects(); changed[1].direction = 6; changed[1].y = 0; changed[3].x = 2; changed[4].duration = 3;
    QVERIFY2(snapshot->replaceEditingSnapshotObjects(changed,false,&error),qPrintable(error));
    QVERIFY2(formal.applyEditingDraft(application(formal,*snapshot),&error),qPrintable(error));
    QCOMPARE(formal.objects()[0].id,before[0].id); QCOMPARE(formal.objects()[0].protectedReason,before[0].protectedReason);
    const QString folder = root.filePath("exported"); QVERIFY(formal.exportSong(folder,&error));
    QJsonObject exported; QVERIFY(ProjectStore::readJson(QDir(folder).filePath("Expert.dat"),&exported,&error));
    QCOMPARE(exported.value("futureMapData"),raw.value("futureMapData"));
    const auto notes = exported.value("_notes").toArray(), originals = raw.value("_notes").toArray();
    QCOMPARE(notes[0],originals[0]);
    QCOMPARE(notes[1].toObject().value("futureObjectData"),originals[1].toObject().value("futureObjectData"));
    QCOMPARE(notes[1].toObject().value("_customData"),originals[1].toObject().value("_customData"));
    QVERIFY(formal.undo()); QCOMPARE(signature(formal),refinementBaselineHash(before));
}

void EditingDraftTest::invalidDraftsAreAtomic() {
    QTemporaryDir root; BeatmapDocument formal; QString error; QJsonObject raw;
    QVERIFY(importFixture(root,&formal,&raw,&error)); auto snapshot = formal.createEditingSnapshot(&error); QVERIFY(snapshot);
    const auto baseline = application(formal,*snapshot); const auto signatureBefore = signature(formal);
    const auto revision = formal.revision();
    QVector<EditingDraftApplication> invalid;
    auto request = baseline; request.expectedRevision--; invalid.append(request);
    request = baseline; request.baselineHash = "changed"; invalid.append(request);
    request = baseline; request.targetDifficultyId = "another"; invalid.append(request);
    request = baseline; request.targetDifficultyName = "Hard"; invalid.append(request);
    request = baseline; request.targetDifficultyRank = 5; invalid.append(request);
    request = baseline; request.objects.removeFirst(); invalid.append(request);
    request = baseline; request.objects[0].protectedReason.clear(); invalid.append(request);
    request = baseline; request.objects[0].y = 0; invalid.append(request);
    request = baseline; request.objects[1].preservedCustomData = {}; invalid.append(request);
    request = baseline; request.objects[2].kind = ObjectKind::Bomb; invalid.append(request);
    request = baseline; request.objects.append(request.objects[2]); invalid.append(request);
    request = baseline; auto collision = note(6,2); collision.id = "new:collision"; request.objects.append(collision); invalid.append(request);
    request = baseline; auto outside = note(12,4); outside.id = "new:invalid"; request.objects.append(outside); invalid.append(request);
    for (const auto &value : invalid) {
        QVERIFY(!formal.applyEditingDraft(value,&error)); QVERIFY(!error.isEmpty());
        QCOMPARE(signature(formal),signatureBefore); QCOMPARE(formal.revision(),revision); QVERIFY(!formal.canUndo());
    }
    // No-op applications retain chart revision and undo history.
    QVERIFY(formal.applyEditingDraft(baseline,&error)); QCOMPARE(formal.revision(),revision); QVERIFY(!formal.canUndo());
}

void EditingDraftTest::restoringSnapshotRevivesIdsAndClearsHistory() {
    QTemporaryDir root; BeatmapDocument formal; QString error;
    QVERIFY(formal.createNew(audio(root),"恢复身份",120,0,{},&error)); QVERIFY(formal.addObject(note(2,0),&error));
    auto snapshot = formal.createEditingSnapshot(&error); QVERIFY(snapshot); const auto initial = snapshot->objects();
    QVERIFY(snapshot->removeObjects({initial[0].id},&error)); QVERIFY(snapshot->addObject(note(4,2),&error));
    QVERIFY(snapshot->canUndo()); QVERIFY(snapshot->replaceEditingSnapshotObjects(initial,true,&error));
    QCOMPARE(signature(*snapshot),refinementBaselineHash(initial)); QVERIFY(!snapshot->canUndo()); QVERIFY(!snapshot->canRedo());
    QVERIFY(!snapshot->isModified());
    auto updated = snapshot->objects().first(); updated.y = 0;
    QVERIFY(snapshot->updateObject(updated,&error)); QVERIFY(snapshot->undo()); QCOMPARE(signature(*snapshot),signature(formal));
}

void EditingDraftTest::newTargetApplicationHasOneUndo() {
    QTemporaryDir root; BeatmapDocument formal; QString error;
    QVERIFY(formal.createNew(audio(root),"新目标",120,0,{},&error)); QVERIFY(formal.addObject(note(2,0),&error));
    QVERIFY(formal.saveProject(root.filePath("saved/project.lmsc"),&error)); QVERIFY(!formal.isModified());
    const auto expert = formal.currentDifficultyId(), before = signature(formal);
    auto snapshot = formal.createEditingSnapshotForDifficulty({},"Hard",5,&error); QVERIFY(snapshot);
    auto extra = note(4,1,1); extra.id = "draft:stable-added";
    QVERIFY(snapshot->replaceEditingSnapshotObjects({extra},true,&error));
    auto request = application(formal,*snapshot); request.targetDifficultyId.clear(); request.targetDifficultyName = "Hard"; request.targetDifficultyRank = 5;
    const auto revision = formal.revision();
    auto invalid = request; invalid.objects[0].x = 4;
    QVERIFY(!formal.applyEditingDraft(invalid,&error)); QCOMPARE(formal.revision(),revision);
    QCOMPARE(formal.difficulties().size(),1); QCOMPARE(signature(formal),before); QVERIFY(!formal.isModified());
    QVERIFY2(formal.applyEditingDraft(request,&error),qPrintable(error));
    QCOMPARE(formal.revision(),revision+1); QCOMPARE(formal.difficulties().size(),2);
    const auto hard = formal.currentDifficultyId(); QCOMPARE(formal.objects().first().id,extra.id);
    QVERIFY(formal.undo()); QCOMPARE(formal.difficulties().size(),1); QCOMPARE(formal.currentDifficultyId(),expert); QCOMPARE(signature(formal),before);
    QVERIFY(!formal.isModified());
    QVERIFY(formal.redo()); QCOMPARE(formal.difficulties().size(),2); QCOMPARE(formal.currentDifficultyId(),hard);
    QCOMPARE(formal.objects().first().id,extra.id);
    request.expectedRevision = formal.revision(); QVERIFY(!formal.applyEditingDraft(request,&error));
    QCOMPARE(formal.difficulties().size(),2);
}

void EditingDraftTest::draftRecordsAreDirtyWithoutChangingFormalRevision() {
    QTemporaryDir root; BeatmapDocument formal; QString error;
    QVERIFY(formal.createNew(audio(root),"草稿持久化",120,0,{},&error)); QVERIFY(formal.addObject(note(2,0),&error));
    const QString project = root.filePath("saved/project.lmsc"); QVERIFY(formal.saveProject(project,&error)); QVERIFY(!formal.isModified());
    const auto revision = formal.revision(); const auto before = signature(formal);
    const QJsonArray first{QJsonObject{{"target","Expert"},{"current",QJsonArray{1,2,3}},{"future",QJsonObject{{"key","keep"}}}}};
    QVERIFY(formal.setEditorDraftRecords(first,&error)); QCOMPARE(formal.revision(),revision); QVERIFY(formal.isModified());
    QVERIFY(formal.autoSave(&error)); QVERIFY(BeatmapDocument::hasRecovery(project)); QVERIFY(formal.isModified());
    BeatmapDocument recovered; QVERIFY(recovered.loadProject(BeatmapDocument::recoveryPath(project),&error));
    QCOMPARE(recovered.editorDraftRecords(),QJsonValue(first)); QCOMPARE(signature(recovered),before); QVERIFY(recovered.isModified());
    QVERIFY(formal.saveProject(project,&error)); QVERIFY(!formal.isModified()); QCOMPARE(formal.revision(),revision);
    QVERIFY(!BeatmapDocument::hasRecovery(project));
    QJsonObject manifest; QVERIFY(ProjectStore::readJson(project,&manifest,&error)); QCOMPARE(manifest.value("version").toInt(),2);
    QCOMPARE(manifest.value("editorDrafts"),QJsonValue(first));
    BeatmapDocument reopened; QVERIFY(reopened.loadProject(project,&error)); QCOMPARE(reopened.editorDraftRecords(),QJsonValue(first)); QVERIFY(!reopened.isModified());
    QVERIFY(reopened.setEditorDraftRecords(first,&error)); QVERIFY(!reopened.isModified());
    QVERIFY(reopened.setEditorDraftRecords(QJsonArray{},&error)); QVERIFY(reopened.isModified());
    QCOMPARE(signature(reopened),before); QVERIFY(!reopened.canUndo());
}

void EditingDraftTest::legacyAndDamagedDraftRecordsRoundTrip() {
    QTemporaryDir root; BeatmapDocument seed; QString error;
    QVERIFY(seed.createNew(audio(root),"兼容工程",120,0,{},&error)); QVERIFY(seed.addObject(note(2,0),&error));
    const QString project = root.filePath("saved/project.lmsc"); QVERIFY(seed.saveProject(project,&error));
    QJsonObject manifest; QVERIFY(ProjectStore::readJson(project,&manifest,&error));
    manifest.insert("version",1); manifest.remove("editorDrafts"); QVERIFY(ProjectStore::writeJson(project,manifest,&error));
    BeatmapDocument legacy; QVERIFY2(legacy.loadProject(project,&error),qPrintable(error));
    QCOMPARE(legacy.editorDraftRecords(),QJsonValue(QJsonArray{})); const auto before = signature(legacy);
    QVERIFY(legacy.saveProject(project,&error)); QVERIFY(ProjectStore::readJson(project,&manifest,&error)); QCOMPARE(manifest.value("version").toInt(),2);
    const QVector<QJsonValue> damaged{QString("broken-json-record"),42,QJsonArray{QJsonObject{{"unknown-version",999},{"objects","damaged"}},false,QJsonValue::Null}};
    for (const auto &value : damaged) {
        manifest.insert("editorDrafts",value); QVERIFY(ProjectStore::writeJson(project,manifest,&error));
        BeatmapDocument reopened; QVERIFY2(reopened.loadProject(project,&error),qPrintable(error));
        QCOMPARE(reopened.editorDraftRecords(),value); QCOMPARE(signature(reopened),before); QVERIFY(!reopened.isModified());
        auto object = reopened.objects()[0]; object.y = 0; QVERIFY(reopened.updateObject(object,&error));
        QVERIFY(reopened.saveProject(project,&error)); QJsonObject saved; QVERIFY(ProjectStore::readJson(project,&saved,&error));
        QCOMPARE(saved.value("editorDrafts"),value);
        // Reset the formal fixture while preserving each malformed record.
        QVERIFY(reopened.undo()); QVERIFY(reopened.saveProject(project,&error));
        QVERIFY(ProjectStore::readJson(project,&manifest,&error));
    }
}

QTEST_GUILESS_MAIN(EditingDraftTest)
#include "EditingDraftTest.moc"
