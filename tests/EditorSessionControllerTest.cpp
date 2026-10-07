#include "gui/EditorSessionController.h"
#include "core/MusicFeatureAnalyzer.h"
#include "core/ProjectStore.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace lmsc;
namespace {
BeatObject note(double beat, int x, int hand = 0, int y = 1) {
    BeatObject object; object.beat = beat; object.x = x; object.y = y;
    object.color = hand; object.direction = 1; return object;
}
QString hash(const BeatmapDocument &document) { return refinementBaselineHash(document.objects()); }
bool initialize(QTemporaryDir &root, BeatmapDocument *document, QString *error) {
    QFile audio(root.filePath("synthetic.ogg"));
    if (!audio.open(QIODevice::WriteOnly) || audio.write("OggS synthetic session fixture") < 0) return false;
    audio.close();
    return document->createNew(audio.fileName(), QStringLiteral("合成会话验证"), 120, .25, {}, error)
        && document->applyGeneratedChart({note(2,1),note(6,2,1)},"Expert",7,document->revision(),error);
}
GenerationRequest request(const BeatmapDocument &document, const QString &name = "Expert") {
    GenerationRequest source; source.documentId = "formal-session"; source.documentRevision = document.revision();
    source.difficultyId = document.currentDifficultyId(); source.timeMap = document.timeMap();
    source.profile = DifficultyProfile::forName(name); source.audio.durationSeconds = 24;
    source.arrangementSeed = 1234567; return source;
}
GenerationDraft generated(const BeatmapDocument &document, const QString &name = "Expert") {
    GenerationDraft draft; draft.source = request(document,name); draft.summary = QStringLiteral("合成生成候选");
    draft.objects = {note(3,0,0,0),note(5,3,1,2),note(9,1,0,0)};
    MusicAnalysis analysis; analysis.audioFingerprint = "synthetic-PCM";
    MusicSegment segment; segment.id = "S0"; segment.startBeat = 0; segment.endBeat = 16;
    segment.activeSeconds = 8; segment.energy = .5; analysis.segments.append(segment);
    draft.arrangement = std::make_shared<const SongArrangementPlan>(SongArrangementPlanner::localPlan(draft.source,analysis));
    draft.source.arrangement = draft.arrangement; return draft;
}
RefinementResult refined(const GenerationDraft &baseline, const QVector<BeatObject> &objects) {
    RefinementResult result; result.source.generation = baseline.source; result.source.baseline = baseline.objects;
    result.source.baselineHash = refinementBaselineHash(baseline.objects);
    result.candidate = baseline; result.candidate.objects = objects; result.candidate.summary = QStringLiteral("合成精修候选");
    result.patch = refinementDifference(baseline.objects,objects); return result;
}
QJsonObject firstRecord(const EditorSessionController &controller) {
    return controller.toJson().toObject().value("records").toArray().first().toObject();
}
QJsonValue replaceFirstRecord(const EditorSessionController &controller, const QJsonObject &row) {
    auto json = controller.toJson().toObject(); auto records = json.value("records").toArray();
    records.replace(0,row); json.insert("records",records); return json;
}
QString oversizedText() {
    // Each legal JSON control character becomes six ASCII bytes (\u0001).
    // Keep the fixture's binary representation below Qt 5's internal size cap.
    return QString(6*1024*1024,QChar(1));
}
}

class EditorSessionControllerTest : public QObject {
    Q_OBJECT
private slots:
    void emptySessionUsesFormalWithoutDirtying();
    void generatedCandidateIsIndependentAndHasStableIds();
    void manualUndoAndApplyAreSeparateHistories();
    void newDifficultyDraftPreservesOtherDifficulties();
    void missingFormalDifficultyShowsItsEmptyBaseline();
    void oneDraftPerTargetSurvivesSwitches();
    void refinementContinuationKeepsOriginalContext();
    void manualChangesRejectLateRefinement();
    void lateGenerationAndTimeChangesAreRejected();
    void formalEditsStaleOnlyTheirTarget();
    void changedAudioAndCropInvalidateDrafts();
    void saveReopenKeepsWorkingInitialBeforeAndPlan();
    void staleReopenedDraftIsReadOnlyAndRetained();
    void corruptAndUnknownRecordsAreRetained();
    void corruptMetadataAndDuplicateTargetsCannotApply();
    void restoredSessionCannotResumeNetworkWork();
    void appliedCandidateIsReadOnlyAndFreshRefinementCanStart();
    void customColorsIdsAndAudioRemainUnchanged();
    void discardAndCheckpointsPreserveFormalChart();
    void oversizedApplyDoesNotTouchFormalOrSession();
    void oversizedSessionMutationsPreserveCandidates();
    void oversizedRefinementResultDoesNotConsumePendingWork();
    void modelDraftMustPassMotionReviewBeforeAtomicApply();
    void modelReviewSurvivesSaveAndRejectsCorruptMetadata();
};

void EditorSessionControllerTest::emptySessionUsesFormalWithoutDirtying() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY2(initialize(root,&document,&error),qPrintable(error)); const auto revision = document.revision();
    EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QCOMPARE(session.formalDocument(),&document); QCOMPARE(session.activeDocument(),&document);
    QCOMPARE(session.currentTargetKey(),QString("Expert")); QVERIFY(!session.hasDraft());
    QVERIFY(!session.isDirty()); QVERIFY(session.warnings().isEmpty()); QVERIFY(!session.viewReadOnly());
    QCOMPARE(document.revision(),revision); QVERIFY(!session.setView(EditorSessionController::View::Initial,&error));
}
void EditorSessionControllerTest::generatedCandidateIsIndependentAndHasStableIds() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); const auto before = hash(document); const auto revision = document.revision();
    EditorSessionController session; session.setFormalDocument(&document,"formal-session"); QSignalSpy spy(&session,&EditorSessionController::changed);
    QVERIFY2(session.acceptGenerationDraft(generated(document),&error),qPrintable(error));
    QCOMPARE(hash(document),before); QCOMPARE(document.revision(),revision); QVERIFY(session.hasDraft());
    QVERIFY(session.activeDocument()!=&document); QVERIFY(session.workingDocument()->isEditingSnapshot());
    QVERIFY(session.isApplicable()); QVERIFY(!session.isManualModified()); QVERIFY(!session.workingDocument()->canUndo());
    QCOMPARE(hash(*session.initialDocument()),hash(*session.workingDocument()));
    QSet<QString> ids; for(const auto &object:session.generationDraft().objects) { QVERIFY(!object.id.isEmpty()); ids.insert(object.id); }
    QCOMPARE(ids.size(),3); QVERIFY(spy.size()>0);
    QVERIFY(session.setView(EditorSessionController::View::Initial,&error)); QVERIFY(session.viewReadOnly());
    QVERIFY(session.setView(EditorSessionController::View::Formal,&error)); QVERIFY(!session.viewReadOnly());
    QVERIFY(session.setView(EditorSessionController::View::Working,&error)); QVERIFY(!session.viewReadOnly());
}
void EditorSessionControllerTest::manualUndoAndApplyAreSeparateHistories() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); const auto formalBefore = hash(document); const auto formalRevision = document.revision();
    EditorSessionController session; session.setFormalDocument(&document,"formal-session"); QVERIFY(session.acceptGenerationDraft(generated(document),&error));
    const auto initial = hash(*session.workingDocument()); auto changed = session.workingDocument()->objects()[0]; changed.y = 1;
    QVERIFY(session.workingDocument()->updateObject(changed,&error)); session.notifyWorkingChanged();
    QVERIFY(session.isManualModified()); QCOMPARE(document.revision(),formalRevision); QCOMPARE(hash(document),formalBefore);
    const auto edited = hash(*session.workingDocument()); QVERIFY(session.undo()); QCOMPARE(hash(*session.workingDocument()),initial);
    QVERIFY(session.redo()); QCOMPARE(hash(*session.workingDocument()),edited);
    QVERIFY2(session.applyDraft(&error),qPrintable(error)); QCOMPARE(hash(document),edited); QVERIFY(session.isApplied());
    QCOMPARE(session.view(),EditorSessionController::View::Formal); QVERIFY(!session.viewReadOnly());
    QVERIFY(document.undo()); QCOMPARE(hash(document),formalBefore); QVERIFY(document.redo()); QCOMPARE(hash(document),edited);
    QVERIFY(!session.applyDraft(&error)); QVERIFY(session.setView(EditorSessionController::View::Working,&error)); QVERIFY(session.viewReadOnly());
}
void EditorSessionControllerTest::newDifficultyDraftPreservesOtherDifficulties() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); const auto expert = document.currentDifficultyId(); const auto before = hash(document);
    EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY2(session.acceptGenerationDraft(generated(document,"Hard"),&error),qPrintable(error));
    QCOMPARE(document.difficulties().size(),1); QCOMPARE(document.currentDifficultyId(),expert); QCOMPARE(hash(document),before);
    QVERIFY2(session.isApplicable(&error),qPrintable(error)); const auto hard = hash(*session.workingDocument());
    QVERIFY2(session.applyDraft(&error),qPrintable(error)); QCOMPARE(document.difficulties().size(),2); QCOMPARE(hash(document),hard);
    QVERIFY(document.undo()); QCOMPARE(document.difficulties().size(),1); QCOMPARE(document.currentDifficultyId(),expert); QCOMPARE(hash(document),before);
    QVERIFY(document.redo()); QCOMPARE(document.difficulties().size(),2); QCOMPARE(hash(document),hard);
    QVERIFY(document.setDifficulty(expert,&error)); QCOMPARE(hash(document),before);
}
void EditorSessionControllerTest::oneDraftPerTargetSurvivesSwitches() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); const auto revision = document.revision();
    EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); const auto expertDraft = hash(*session.workingDocument());
    QVERIFY(session.acceptGenerationDraft(generated(document,"Hard"),&error)); const auto hardDraft = hash(*session.workingDocument());
    QCOMPARE(session.targetKeys().size(),2); QVERIFY(session.selectTarget("Expert",&error)); QCOMPARE(hash(*session.workingDocument()),expertDraft);
    QVERIFY(session.isApplicable()); QVERIFY(session.selectTarget("Hard",&error)); QCOMPARE(hash(*session.workingDocument()),hardDraft);
    QVERIFY(session.acceptGenerationDraft(generated(document,"Hard"),&error)); QCOMPARE(session.targetKeys().size(),2);
    QCOMPARE(document.revision(),revision);
}
void EditorSessionControllerTest::missingFormalDifficultyShowsItsEmptyBaseline() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); const auto expert=document.currentDifficultyId();
    EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document,"Hard"),&error));
    QVERIFY(session.setView(EditorSessionController::View::Formal,&error));
    QVERIFY(session.activeDocument()!=&document); QVERIFY(session.activeDocument()->isEditingSnapshot());
    QVERIFY(session.activeDocument()->objects().isEmpty()); QVERIFY(session.viewReadOnly());
    QCOMPARE(document.currentDifficultyId(),expert); QCOMPARE(document.objects().size(),2);
    const auto target=session.activeDocument()->currentDifficultyId();
    for(const auto &difficulty:session.activeDocument()->difficulties()) if(difficulty.id==target) QCOMPARE(difficulty.name,QString("Hard"));
    QVERIFY(session.setView(EditorSessionController::View::Working,&error)); QVERIFY(!session.viewReadOnly());
}
void EditorSessionControllerTest::refinementContinuationKeepsOriginalContext() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.beginRefinementDraft(request(document),&error)); const auto original = session.generationDraft();
    QVERIFY(session.beginRefinement(false,&error)); const auto before = hash(*session.beforeRefinementDocument());
    auto first = original.objects; first[0].y = 0; auto result = refined(original,first); result.resumable = true;
    QVERIFY2(session.mergeRefinementResult(result,&error),qPrintable(error)); QCOMPARE(hash(*session.beforeRefinementDocument()),before);
    QVERIFY(session.beginRefinement(true,&error)); auto second = first; second[1].y = 2;
    // A continued result deliberately carries the FIRST baseline, not first[] above.
    QVERIFY2(session.mergeRefinementResult(refined(original,second),&error),qPrintable(error));
    QCOMPARE(hash(*session.beforeRefinementDocument()),before); QCOMPARE(session.generationDraft().objects[1].y,2);
    QVERIFY(session.restoreInitial(&error)); QCOMPARE(hash(*session.workingDocument()),refinementBaselineHash(original.objects));
    QVERIFY(session.undo()); QCOMPARE(hash(*session.workingDocument()),refinementBaselineHash(second));
}
void EditorSessionControllerTest::manualChangesRejectLateRefinement() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); const auto baseline = session.generationDraft();
    QVERIFY(session.beginRefinement(false,&error)); auto objects = baseline.objects; objects[0].direction = 6;
    auto changed = session.workingDocument()->objects()[1]; changed.y = 1;
    QVERIFY(session.workingDocument()->updateObject(changed,&error)); session.notifyWorkingChanged(); const auto manual = hash(*session.workingDocument());
    QVERIFY(!session.mergeRefinementResult(refined(baseline,objects),&error)); QCOMPARE(hash(*session.workingDocument()),manual);
    QVERIFY(!session.beginRefinement(true,&error));
}
void EditorSessionControllerTest::lateGenerationAndTimeChangesAreRejected() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    auto draft = generated(document); auto wrong = draft; wrong.source.documentId = "other-document";
    QVERIFY(!session.acceptGenerationDraft(wrong,&error)); QVERIFY(!session.hasDraft());
    QVERIFY(document.addObject(note(10,0),&error)); QVERIFY(!session.acceptGenerationDraft(draft,&error)); QVERIFY(!session.hasDraft());
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); QVERIFY(document.setNewSongTempo(130,.4,&error));
    QVERIFY(!session.isApplicable()); QVERIFY(session.viewReadOnly()); const auto before = hash(document);
    QVERIFY(!session.applyDraft(&error)); QCOMPARE(hash(document),before);
}
void EditorSessionControllerTest::formalEditsStaleOnlyTheirTarget() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); const auto expert = document.currentDifficultyId();
    QVERIFY(document.applyGeneratedChart({note(4,0)},"Easy",1,document.revision(),&error)); const auto easy = document.currentDifficultyId();
    QVERIFY(document.setDifficulty(expert,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error));
    QVERIFY(document.setDifficulty(easy,&error)); auto other = document.objects()[0]; other.y = 0; QVERIFY(document.updateObject(other,&error));
    QVERIFY2(session.isApplicable(&error),qPrintable(error)); QVERIFY(document.setDifficulty(expert,&error)); QVERIFY(session.isApplicable());
    QVERIFY(session.setView(EditorSessionController::View::Formal,&error)); QVERIFY(!session.viewReadOnly());
    auto changed = document.objects()[0]; changed.y = 0; QVERIFY(document.updateObject(changed,&error));
    QVERIFY(session.setView(EditorSessionController::View::Working,&error)); QVERIFY(session.viewReadOnly()); QVERIFY(!session.isApplicable());
}
void EditorSessionControllerTest::changedAudioAndCropInvalidateDrafts() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); QFile audio(document.audioPath());
    QVERIFY(audio.open(QIODevice::Append)); QVERIFY(audio.write(" changed")>0); audio.close();
    QVERIFY(!session.isApplicable()); QVERIFY(!session.applyDraft(&error));
    QTemporaryDir sourceRoot; BeatmapDocument sourceDocument; QVERIFY(initialize(sourceRoot,&sourceDocument,&error));
    QFile source(sourceRoot.filePath("synthetic-source.mp3")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("synthetic source"); source.close();
    QVERIFY(sourceDocument.setImportSource(source.fileName(),0,0,20,&error)); EditorSessionController second;
    second.setFormalDocument(&sourceDocument,"formal-session"); QVERIFY(second.acceptGenerationDraft(generated(sourceDocument),&error));
    QVERIFY(sourceDocument.setImportSource(source.fileName(),0,1,20,&error)); QVERIFY(!second.isApplicable());
}
void EditorSessionControllerTest::saveReopenKeepsWorkingInitialBeforeAndPlan() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); const auto initial = hash(*session.initialDocument());
    QVERIFY(session.beginRefinement(false,&error)); const auto before = hash(*session.beforeRefinementDocument()); auto baseline = session.generationDraft();
    auto edited = baseline.objects; edited[0].direction = 6; QVERIFY(session.mergeRefinementResult(refined(baseline,edited),&error));
    auto handEdit = session.workingDocument()->objects()[1]; handEdit.y = 1;
    QVERIFY(session.workingDocument()->updateObject(handEdit,&error)); session.notifyWorkingChanged(); const auto working = hash(*session.workingDocument());
    const auto path = root.filePath("saved/project.lmsc"); QVERIFY2(document.saveProject(path,&error),qPrintable(error)); session.markSaved(); QVERIFY(!session.isDirty());
    BeatmapDocument reopened; QVERIFY2(reopened.loadProject(path,&error),qPrintable(error)); EditorSessionController restored;
    restored.setFormalDocument(&reopened,"new-runtime-id");
    QVERIFY2(restored.isApplicable(&error),qPrintable(error)); QVERIFY(!restored.isDirty());
    QCOMPARE(hash(*restored.workingDocument()),working); QCOMPARE(hash(*restored.initialDocument()),initial);
    QCOMPARE(hash(*restored.beforeRefinementDocument()),before); QVERIFY(restored.isManualModified());
    const auto draft = restored.generationDraft(); QVERIFY(draft.source.audio.path.isEmpty()); QVERIFY(!draft.source.audio.lease);
    QCOMPARE(draft.source.arrangementSeed,quint32(1234567)); QVERIFY(draft.arrangement);
    QCOMPARE(draft.arrangement->source,QString("local")); QCOMPARE(draft.arrangement->toJson(),session.generationDraft().arrangement->toJson());
    QCOMPARE(draft.source.documentId,QString("new-runtime-id"));
    QVERIFY2(restored.applyDraft(&error),qPrintable(error)); QCOMPARE(hash(reopened),working);
}
void EditorSessionControllerTest::staleReopenedDraftIsReadOnlyAndRetained() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); const auto candidate = hash(*session.workingDocument());
    auto change = document.objects()[0]; change.y = 0; QVERIFY(document.updateObject(change,&error));
    const auto path = root.filePath("saved/project.lmsc"); QVERIFY(document.saveProject(path,&error));
    BeatmapDocument reopened; QVERIFY(reopened.loadProject(path,&error)); EditorSessionController restored; restored.setFormalDocument(&reopened,"formal-session");
    QVERIFY(restored.hasDraft()); QCOMPARE(hash(*restored.workingDocument()),candidate); QVERIFY(restored.viewReadOnly());
    QVERIFY(!restored.isApplicable()); QVERIFY(!restored.warnings().isEmpty()); const auto formal = hash(reopened);
    QVERIFY(!restored.applyDraft(&error)); QCOMPARE(hash(reopened),formal);
    QVERIFY(restored.toJson().toObject().value("records").toArray().size()==1);
}
void EditorSessionControllerTest::corruptAndUnknownRecordsAreRetained() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    const QJsonValue bad = QJsonObject{{"version",99},{"private-extension",QJsonArray{"opaque",13}}};
    QStringList warnings; QVERIFY(!session.restoreFromJson(bad,&warnings)); QVERIFY(!warnings.isEmpty()); QCOMPARE(session.toJson(),bad);
    QVERIFY(session.synchronizeRecords(&error)); const auto path=root.filePath("saved/project.lmsc"); QVERIFY(document.saveProject(path,&error));
    BeatmapDocument reopened; QVERIFY(reopened.loadProject(path,&error)); QCOMPARE(reopened.editorDraftRecords(),bad);
    QVERIFY(session.acceptGenerationDraft(generated(document),&error));
    QCOMPARE(session.toJson().toObject().value("unrecognizedRecords"),bad);
    auto row=firstRecord(session); row.insert("unrecognized-field",QJsonObject{{"keep",true}});
    auto working=row.value("working").toArray(); auto object=working[0].toObject(); object.insert("direction",99); working.replace(0,object); row.insert("working",working);
    QVERIFY(session.restoreFromJson(replaceFirstRecord(session,row))); QVERIFY(session.hasDraft()); QVERIFY(!session.isApplicable());
    QCOMPARE(firstRecord(session),row); QVERIFY(!session.warnings().isEmpty()); QVERIFY(!session.applyDraft(&error));
}
void EditorSessionControllerTest::corruptMetadataAndDuplicateTargetsCannotApply() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); const auto valid = session.toJson();
    auto row=firstRecord(session); auto metadata=row.value("metadata").toObject(); auto plan=metadata.value("plan").toObject();
    auto sections=plan.value("sections").toArray(); auto section=sections[0].toObject(); section.insert("family","not-an-action");
    sections.replace(0,section); plan.insert("sections",sections); metadata.insert("plan",plan); row.insert("metadata",metadata);
    session.restoreFromJson(replaceFirstRecord(session,row)); QVERIFY(!session.isApplicable()); QCOMPARE(firstRecord(session),row);
    QVERIFY(session.restoreFromJson(valid)); auto duplicate=valid.toObject(); auto rows=duplicate.value("records").toArray(); rows.append(rows[0]); duplicate.insert("records",rows);
    QVERIFY(!session.restoreFromJson(duplicate)); QVERIFY(!session.isApplicable()); QVERIFY(!session.applyDraft(&error));
}
void EditorSessionControllerTest::restoredSessionCannotResumeNetworkWork() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); QVERIFY(session.beginRefinement(false,&error));
    const auto saved=session.toJson(); QVERIFY(!QJsonDocument(saved.toObject()).toJson().contains("jobId"));
    QVERIFY(session.restoreFromJson(saved)); QVERIFY(!session.beginRefinement(true,&error));
    QVERIFY(session.beginRefinement(false,&error));
}
void EditorSessionControllerTest::appliedCandidateIsReadOnlyAndFreshRefinementCanStart() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); QVERIFY(session.applyDraft(&error));
    QVERIFY(session.isApplied()); QVERIFY(!session.isApplicable()); QVERIFY(session.setView(EditorSessionController::View::Initial,&error)); QVERIFY(session.viewReadOnly());
    QVERIFY(session.setView(EditorSessionController::View::Working,&error)); QVERIFY(session.viewReadOnly());
    QVERIFY(session.beginRefinementDraft(request(document),&error)); QVERIFY(!session.isApplied()); QVERIFY(session.isApplicable());
    QCOMPARE(hash(*session.workingDocument()),hash(document)); QVERIFY(!session.hasBeforeRefinement());
}
void EditorSessionControllerTest::customColorsIdsAndAudioRemainUnchanged() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); QVector<BeatObject> objects{note(2,1),note(6,2,1)};
    objects[0].preservedCustomData={{"_customData",QJsonObject{{"_color",QJsonArray{.2,.3,.4,1}}}}};
    QVERIFY2(document.applyGeneratedChart(objects,"Expert",7,document.revision(),&error),qPrintable(error));
    QFile audio(document.audioPath()); QVERIFY(audio.open(QIODevice::ReadOnly)); const auto bytes=audio.readAll(); audio.close();
    EditorSessionController session; session.setFormalDocument(&document,"formal-session"); QVERIFY(session.beginRefinementDraft(request(document),&error));
    const auto before=session.generationDraft(); QVERIFY(session.beginRefinement(false,&error)); auto edited=before.objects; edited[0].y=0;
    QVERIFY(session.mergeRefinementResult(refined(before,edited),&error)); QVERIFY(session.applyDraft(&error));
    QCOMPARE(document.objects()[0].id,before.objects[0].id); QCOMPARE(document.objects()[0].preservedCustomData,before.objects[0].preservedCustomData);
    QVERIFY(audio.open(QIODevice::ReadOnly)); QCOMPARE(audio.readAll(),bytes);
}
void EditorSessionControllerTest::discardAndCheckpointsPreserveFormalChart() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); const auto formal=hash(document); const auto revision=document.revision();
    EditorSessionController session; session.setFormalDocument(&document,"formal-session"); QVERIFY(session.acceptGenerationDraft(generated(document),&error));
    QVERIFY(session.isDirty()); session.markSaved(); QVERIFY(!session.isDirty()); QCOMPARE(session.toJson(),session.serializedCheckpoint());
    auto changed=session.workingDocument()->objects()[0]; changed.y=1; QVERIFY(session.workingDocument()->updateObject(changed,&error)); session.notifyWorkingChanged();
    QVERIFY(session.isDirty()); QCOMPARE(document.revision(),revision); QVERIFY(session.discardDraft(&error));
    QVERIFY(!session.hasDraft()); QCOMPARE(session.activeDocument(),&document); QCOMPARE(hash(document),formal); QCOMPARE(document.revision(),revision);
}
void EditorSessionControllerTest::oversizedApplyDoesNotTouchFormalOrSession() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); auto row=firstRecord(session);
    row.insert("opaqueExtension",oversizedText());
    QVERIFY(QJsonDocument(row).toJson(QJsonDocument::Compact).size()>32*1024*1024);
    QVERIFY(session.restoreFromJson(replaceFirstRecord(session,row))); QVERIFY(session.isApplicable());
    const auto formal=hash(document), working=hash(*session.workingDocument()); const auto revision=document.revision();
    const auto records=document.editorDraftRecords(), candidate=session.toJson();
    const auto view=session.view(); const auto target=session.currentTargetKey();
    const bool canUndo=document.canUndo(), canRedo=document.canRedo(), dirty=session.isDirty();
    QVERIFY(!session.applyDraft(&error)); QVERIFY(error.contains("32 MiB"));
    QCOMPARE(hash(document),formal); QCOMPARE(document.revision(),revision); QCOMPARE(document.canUndo(),canUndo); QCOMPARE(document.canRedo(),canRedo);
    QCOMPARE(document.editorDraftRecords(),records); QCOMPARE(session.toJson(),candidate); QCOMPARE(session.isDirty(),dirty);
    QCOMPARE(hash(*session.workingDocument()),working); QCOMPARE(session.view(),view); QCOMPARE(session.currentTargetKey(),target);
    QVERIFY(!session.isApplied()); QVERIFY(session.isApplicable());
}
void EditorSessionControllerTest::oversizedSessionMutationsPreserveCandidates() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); QVERIFY(session.beginRefinement(false,&error));
    const auto before=hash(*session.beforeRefinementDocument()); auto changed=session.workingDocument()->objects()[0]; changed.direction=6;
    QVERIFY(session.workingDocument()->updateObject(changed,&error)); session.notifyWorkingChanged();
    const auto expert=hash(*session.workingDocument()); QVERIFY(session.acceptGenerationDraft(generated(document,"Hard"),&error));
    const auto hard=hash(*session.workingDocument()); QVERIFY(session.selectTarget("Expert",&error));
    auto row=firstRecord(session); QCOMPARE(row.value("target").toString(),QString("Expert"));
    row.insert("opaqueExtension",oversizedText());
    QVERIFY(QJsonDocument(row).toJson(QJsonDocument::Compact).size()>32*1024*1024);
    QVERIFY(session.restoreFromJson(replaceFirstRecord(session,row))); const auto candidate=session.toJson();
    const auto formal=hash(document); const auto revision=document.revision(); const auto records=document.editorDraftRecords();
    QVERIFY(!session.acceptGenerationDraft(generated(document,"Hard"),&error)); QVERIFY(error.contains("32 MiB"));
    QCOMPARE(session.currentTargetKey(),QString("Expert")); QCOMPARE(session.toJson(),candidate); QCOMPARE(session.targetKeys().size(),2);
    QVERIFY(!session.beginRefinement(false,&error)); QCOMPARE(hash(*session.beforeRefinementDocument()),before);
    QVERIFY(!session.restoreInitial(&error)); QCOMPARE(hash(*session.workingDocument()),expert); QVERIFY(!session.workingDocument()->canUndo());
    QCOMPARE(session.toJson(),candidate); QVERIFY(session.selectTarget("Hard",&error)); const auto beforeDiscard=session.toJson();
    QVERIFY(!session.discardDraft(&error)); QCOMPARE(session.targetKeys().size(),2); QCOMPARE(hash(*session.workingDocument()),hard);
    QCOMPARE(session.view(),EditorSessionController::View::Working); QCOMPARE(session.toJson(),beforeDiscard);
    QCOMPARE(hash(document),formal); QCOMPARE(document.revision(),revision); QCOMPARE(document.editorDraftRecords(),records);
}
void EditorSessionControllerTest::oversizedRefinementResultDoesNotConsumePendingWork() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    QVERIFY(session.acceptGenerationDraft(generated(document),&error)); QVERIFY(session.beginRefinement(false,&error));
    const auto baseline=session.generationDraft(); auto objects=baseline.objects; objects[0].direction=6;
    auto result=refined(baseline,objects); result.candidate.summary=oversizedText();
    const auto formal=hash(document), working=hash(*session.workingDocument()); const auto revision=document.revision();
    const auto workingRevision=session.workingDocument()->revision(); const auto records=document.editorDraftRecords(); const auto candidate=session.toJson();
    QVERIFY(!session.mergeRefinementResult(result,&error)); QVERIFY(error.contains("32 MiB"));
    QCOMPARE(hash(document),formal); QCOMPARE(document.revision(),revision); QCOMPARE(hash(*session.workingDocument()),working);
    QCOMPARE(session.workingDocument()->revision(),workingRevision); QCOMPARE(document.editorDraftRecords(),records); QCOMPARE(session.toJson(),candidate);
    QVERIFY(!session.workingDocument()->canUndo());
    result.candidate.summary=QStringLiteral("在同一精修请求中重试合规结果");
    QVERIFY2(session.mergeRefinementResult(result,&error),qPrintable(error)); QCOMPARE(hash(*session.workingDocument()),refinementBaselineHash(objects));
}

void EditorSessionControllerTest::modelDraftMustPassMotionReviewBeforeAtomicApply() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error));
    EditorSessionController session; session.setFormalDocument(&document,"formal-session");
    auto draft = generated(document); draft.requiresPlayabilityReview = true; draft.playabilityActiveSeconds = 20;
    draft.objects = {note(2,1),note(3,1)}; // Legal gap and connection, but a same-direction backstroke.
    const auto formal = hash(document); const auto revision = document.revision();
    QVERIFY2(session.acceptGenerationDraft(draft,&error),qPrintable(error));
    QVERIFY(session.isApplicable()); QVERIFY(!session.viewReadOnly());
    QVERIFY(!session.canApplyDraft(&error)); QVERIFY(error.contains("动作检查"));
    QVERIFY(error.contains("同向回刀"));
    QVERIFY(!session.applyDraft(&error)); QCOMPARE(hash(document),formal); QCOMPARE(document.revision(),revision);
    const auto badId = session.workingDocument()->objects().last().id;
    QVERIFY(session.workingDocument()->removeObjects({badId},&error)); session.notifyWorkingChanged();
    QVERIFY2(session.canApplyDraft(&error),qPrintable(error));
    const auto repaired = hash(*session.workingDocument());
    QVERIFY2(session.applyDraft(&error),qPrintable(error)); QCOMPARE(hash(document),repaired);
    QVERIFY(document.undo()); QCOMPARE(hash(document),formal);
    QVERIFY(document.redo()); QCOMPARE(hash(document),repaired);
}

void EditorSessionControllerTest::modelReviewSurvivesSaveAndRejectsCorruptMetadata() {
    QTemporaryDir root; BeatmapDocument document; QString error;
    QVERIFY(initialize(root,&document,&error)); EditorSessionController session;
    session.setFormalDocument(&document,"formal-session");
    auto draft = generated(document); draft.requiresPlayabilityReview = true; draft.playabilityActiveSeconds = 18;
    draft.objects = {note(2,1),note(2.1,1)};
    QVERIFY(session.acceptGenerationDraft(draft,&error));
    QVERIFY(session.beginRefinement(false,&error));
    auto result = refined(session.generationDraft(),session.generationDraft().objects);
    result.candidate.requiresPlayabilityReview = false; result.candidate.playabilityActiveSeconds = 0;
    QVERIFY(session.mergeRefinementResult(result,&error));
    QVERIFY(session.generationDraft().requiresPlayabilityReview);
    QCOMPARE(session.generationDraft().playabilityActiveSeconds,18.0);
    const auto project = root.filePath("model/project.lmsc");
    QVERIFY2(document.saveProject(project,&error),qPrintable(error));
    BeatmapDocument reopened; QVERIFY2(reopened.loadProject(project,&error),qPrintable(error));
    EditorSessionController restored; restored.setFormalDocument(&reopened,"formal-session");
    QVERIFY(restored.generationDraft().requiresPlayabilityReview);
    QCOMPARE(restored.generationDraft().playabilityActiveSeconds,18.0);
    QVERIFY(restored.isApplicable()); QVERIFY(!restored.canApplyDraft(&error)); QVERIFY(!restored.applyDraft(&error));
    auto row = firstRecord(restored); auto metadata = row.value("metadata").toObject();
    metadata.insert("requiresPlayabilityReview","false"); row.insert("metadata",metadata);
    QVERIFY(restored.restoreFromJson(replaceFirstRecord(restored,row)));
    QVERIFY(!restored.isApplicable()); QCOMPARE(firstRecord(restored),row);
}

QTEST_GUILESS_MAIN(EditorSessionControllerTest)
#include "EditorSessionControllerTest.moc"
