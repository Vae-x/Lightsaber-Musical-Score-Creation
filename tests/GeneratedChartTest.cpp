#include "core/BeatmapDocument.h"
#include "core/ProjectStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <limits>

using namespace lmsc;

namespace {
QString audioFixture(QTemporaryDir &root) {
    const QString path = root.filePath("source.ogg");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("OggS synthetic metadata-only fixture") < 0) return {};
    return path;
}
QString chartSignature(const BeatmapDocument &document) {
    QString result;
    for (const auto &difficulty : document.difficulties()) {
        if (difficulty.id != document.currentDifficultyId()) continue;
        result = difficulty.id + '|' + difficulty.name + '|' + QString::number(difficulty.rank);
        break;
    }
    for (const auto &o : document.objects())
        result += QStringLiteral("|%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11")
            .arg(o.id).arg(int(o.kind)).arg(o.beat, 0, 'g', 17).arg(o.x).arg(o.y)
            .arg(o.color).arg(o.direction).arg(o.duration, 0, 'g', 17).arg(o.width).arg(o.height).arg(o.protectedReason);
    return result;
}
QString difficultyId(const BeatmapDocument &document, const QString &name) {
    for (const auto &difficulty : document.difficulties())
        if (difficulty.name == name) return difficulty.id;
    return {};
}
QMap<QString, QString> allChartSignatures(BeatmapDocument &document) {
    QMap<QString, QString> result;
    const QString selected = document.currentDifficultyId();
    const auto difficulties = document.difficulties();
    for (const auto &difficulty : difficulties) {
        if (!document.setDifficulty(difficulty.id)) return {};
        result.insert(difficulty.name, chartSignature(document));
    }
    if (!document.setDifficulty(selected)) return {};
    return result;
}
QMap<QString, QByteArray> assetBytes(const QString &folder) {
    QMap<QString, QByteArray> result;
    for (const auto &relative : ProjectStore::files(folder)) {
        QFile file(QDir(folder).filePath(relative));
        if (!file.open(QIODevice::ReadOnly)) return {};
        result.insert(relative, QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    return result;
}
BeatObject note(double beat, int lane, int hand = 0) {
    BeatObject object;
    object.beat = beat; object.x = lane; object.y = 1;
    object.color = hand; object.direction = 1;
    return object;
}
}

class GeneratedChartTest : public QObject {
    Q_OBJECT
private slots:
    void wholeChartAndDifficultyUndoTogether();
    void generatingAnotherDifficultyKeepsEarlierCharts();
    void handwrittenChartsSurviveOtherDifficultyGeneration();
    void difficultyRenameAndSwitchPreserveCharts();
    void songTempoAppliesToEveryDifficulty();
    void invalidAndStaleApplicationsAreAtomic();
    void allDifficultyExportsAndProjectRecovery();
    void autosaveRecoversNewDifficultyAfterSingleChartSave();
    void corruptedMultiDifficultyProjectsAreRejectedAtomically();
    void legacySingleDifficultyProjectsRemainEditable();
    void legacyDifficultyCaseMatchesGeneratedTarget();
    void generatedExportDoesNotOverwriteUnreferencedAsset();
    void importedChartsCannotBeReplaced();
    void revisionsNeverReturnToSavedValues();
};

void GeneratedChartTest::wholeChartAndDifficultyUndoTogether() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY2(doc.createNew(audioFixture(root), "原歌名", 120, 0.25, {}, &error), qPrintable(error));
    const QString before = chartSignature(doc);
    const QString originalId = doc.currentDifficultyId();
    const quint64 revision = doc.revision();
    QVector<BeatObject> candidate{note(4, 1), note(6, 2, 1)};
    candidate[0].id = "caller-owned-id";
    QVERIFY2(doc.applyGeneratedChart(candidate, "Hard", 5, revision, &error), qPrintable(error));
    QCOMPARE(doc.objects().size(), 2);
    QCOMPARE(doc.difficulties().size(), 1);
    QCOMPARE(doc.difficulties().first().name, QString("Hard"));
    QCOMPARE(doc.currentDifficultyId(), originalId);
    QVERIFY(doc.objects()[0].id != candidate[0].id);
    QVERIFY(doc.revision() > revision);
    const QString generated = chartSignature(doc);
    QVERIFY(doc.undo());
    QCOMPARE(chartSignature(doc), before);
    QVERIFY(doc.redo());
    QCOMPARE(chartSignature(doc), generated);
    QVERIFY(doc.setNewSongMetadata("之后改的歌名", "作者", "编谱", &error));
    QVERIFY(doc.undo());
    QCOMPARE(doc.title(), QString("之后改的歌名"));
    QCOMPARE(doc.difficulties().first().name, QString("Expert"));
    QVERIFY(doc.objects().isEmpty());
    QVERIFY(doc.isModified());
}

void GeneratedChartTest::generatingAnotherDifficultyKeepsEarlierCharts() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "多难度", 120, 0.25, {}, &error));
    QVERIFY(doc.applyGeneratedChart({note(1, 0), note(3, 3, 1)}, "Easy", 1, doc.revision(), &error));
    const QString easyId = doc.currentDifficultyId();
    const QString easy = chartSignature(doc);
    QVERIFY2(doc.applyGeneratedChart({note(2, 1), note(4, 2, 1)}, "Hard", 5, doc.revision(), &error), qPrintable(error));
    const QString hardId = doc.currentDifficultyId();
    const QString hard = chartSignature(doc);
    QCOMPARE(doc.difficulties().size(), 2);
    QVERIFY(hardId != easyId);
    QVERIFY(doc.setDifficulty(easyId, &error));
    QCOMPARE(chartSignature(doc), easy);

    // New-song commands are global even when another chart is selected.
    QVERIFY(doc.undo());
    QCOMPARE(doc.difficulties().size(), 1);
    QCOMPARE(doc.currentDifficultyId(), easyId);
    QCOMPARE(chartSignature(doc), easy);
    QVERIFY(doc.redo());
    QCOMPARE(doc.currentDifficultyId(), hardId);
    QCOMPARE(chartSignature(doc), hard);

    // Rebuilding an existing difficulty replaces only that chart and selects it.
    QVERIFY(doc.applyGeneratedChart({note(8, 1)}, "Easy", 1, doc.revision(), &error));
    QCOMPARE(doc.difficulties().size(), 2);
    QCOMPARE(doc.currentDifficultyId(), easyId);
    QCOMPARE(doc.objects().size(), 1);
    const QString rebuilt = chartSignature(doc);
    const auto rebuiltCharts = allChartSignatures(doc);
    QCOMPARE(rebuiltCharts.value("Hard"), hard);
    QVERIFY(doc.undo());
    QCOMPARE(doc.currentDifficultyId(), easyId);
    QCOMPARE(allChartSignatures(doc).value("Easy"), easy);
    QCOMPARE(chartSignature(doc), easy);
    QVERIFY(doc.redo());
    QCOMPARE(doc.currentDifficultyId(), easyId);
    QCOMPARE(chartSignature(doc), rebuilt);
    QCOMPARE(allChartSignatures(doc).value("Hard"), hard);

    QVERIFY(doc.setDifficulty(hardId, &error));
    QVERIFY(doc.addObject(note(9, 0), &error));
    QVERIFY(doc.setDifficulty(easyId, &error));
    QVERIFY(doc.undo());
    QCOMPARE(allChartSignatures(doc).value("Hard"), hard);
    QCOMPARE(allChartSignatures(doc).value("Easy"), rebuilt);
}

void GeneratedChartTest::handwrittenChartsSurviveOtherDifficultyGeneration() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "保留手工谱", 120, 0, {}, &error));
    QVERIFY(doc.addObject(note(1, 0), &error));
    QVERIFY(doc.addObject(note(2, 3, 1), &error));
    const QString expertId = doc.currentDifficultyId();
    const QString handmade = chartSignature(doc);
    const QString handmadeObjectId = doc.objects().first().id;
    QVERIFY(doc.applyGeneratedChart({note(4, 1)}, "Easy", 1, doc.revision(), &error));
    QCOMPARE(doc.difficulties().size(), 2);
    QCOMPARE(allChartSignatures(doc).value("Expert"), handmade);
    QVERIFY(doc.undo());
    QCOMPARE(doc.currentDifficultyId(), expertId);
    QCOMPARE(doc.difficulties().size(), 1);
    QCOMPARE(chartSignature(doc), handmade);
    QVERIFY(doc.redo());
    const QString easy = chartSignature(doc);
    auto replacement = note(10, 2, 1);
    replacement.id = handmadeObjectId;
    QVERIFY(doc.applyGeneratedChart({replacement}, "Expert", 7, doc.revision(), &error));
    QCOMPARE(doc.currentDifficultyId(), expertId);
    QVERIFY(doc.objects().first().id != handmadeObjectId);
    QCOMPARE(allChartSignatures(doc).value("Easy"), easy);
    QVERIFY(doc.undo());
    QCOMPARE(doc.currentDifficultyId(), expertId);
    QCOMPARE(allChartSignatures(doc).value("Expert"), handmade);
    QCOMPARE(allChartSignatures(doc).value("Easy"), easy);
}

void GeneratedChartTest::difficultyRenameAndSwitchPreserveCharts() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "修改难度", 120, 0, {}, &error));
    QVERIFY(doc.applyGeneratedChart({note(1, 0)}, "Easy", 1, doc.revision(), &error));
    const QString easyId = doc.currentDifficultyId();
    QVERIFY(doc.applyGeneratedChart({note(4, 3, 1)}, "Hard", 5, doc.revision(), &error));
    const QString hardId = doc.currentDifficultyId();
    const QString hard = chartSignature(doc);
    QVERIFY(doc.setDifficulty(easyId, &error));
    const auto easyObjects = doc.objects();
    QVERIFY(doc.setNewSongDifficulty("Normal", 3, &error));
    QCOMPARE(doc.difficulties().size(), 2);
    QCOMPARE(difficultyId(doc, "Normal"), easyId);
    QVERIFY(difficultyId(doc, "Easy").isEmpty());
    QCOMPARE(doc.objects().first().id, easyObjects.first().id);
    QCOMPARE(allChartSignatures(doc).value("Hard"), hard);
    QVERIFY(doc.undo());
    QCOMPARE(difficultyId(doc, "Easy"), easyId);
    QVERIFY(difficultyId(doc, "Normal").isEmpty());
    const auto before = allChartSignatures(doc);
    QVERIFY(doc.setNewSongDifficulty("Hard", 5, &error));
    QCOMPARE(doc.currentDifficultyId(), hardId);
    QCOMPARE(allChartSignatures(doc), before);
}

void GeneratedChartTest::songTempoAppliesToEveryDifficulty() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "同步对拍", 120, 0, {}, &error));
    QVERIFY(doc.applyGeneratedChart({note(4, 0)}, "Easy", 1, doc.revision(), &error));
    QVERIFY(doc.applyGeneratedChart({note(8, 3, 1)}, "Hard", 5, doc.revision(), &error));
    const auto before = allChartSignatures(doc);
    QVERIFY(doc.setNewSongTempo(150, 0.5, &error));
    const auto difficulties = doc.difficulties();
    for (const auto &difficulty : difficulties) {
        QVERIFY(doc.setDifficulty(difficulty.id, &error));
        QCOMPARE(doc.timeMap().baseBpm(), 150.0);
        QCOMPARE(doc.timeMap().firstBeatSeconds(), 0.5);
        QCOMPARE(doc.timeMap().beatToSeconds(5), 2.5);
        QCOMPARE(chartSignature(doc), before.value(difficulty.name));
    }
}

void GeneratedChartTest::invalidAndStaleApplicationsAreAtomic() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "校验", 120, 0, {}, &error));
    QVERIFY(doc.addObject(note(1, 0), &error));
    QVERIFY(doc.applyGeneratedChart({note(2, 3, 1)}, "Normal", 3, doc.revision(), &error));
    QVERIFY(doc.saveProject(root.filePath("saved/project.lmsc"), &error));
    const auto before = allChartSignatures(doc);
    const QString selected = doc.currentDifficultyId();
    quint64 revision = doc.revision();
    auto reject = [&](QVector<BeatObject> objects, const QString &name, int rank, quint64 expected) {
        QVERIFY(!doc.applyGeneratedChart(objects, name, rank, expected, &error));
        QCOMPARE(doc.revision(), revision);
        QCOMPARE(allChartSignatures(doc), before);
        QCOMPARE(doc.currentDifficultyId(), selected);
        // Switching through each difficulty for inspection advances the revision.
        revision = doc.revision();
        QVERIFY(!doc.isModified());
        QVERIFY(doc.canUndo());
        QVERIFY(!doc.canRedo());
    };
    reject({note(4, 1), note(4, 1, 1)}, "Easy", 1, revision);
    auto invalid = note(4, 1); invalid.direction = 9;
    reject({invalid}, "Easy", 1, revision);
    invalid = note(4, 1); invalid.beat = std::numeric_limits<double>::quiet_NaN();
    reject({invalid}, "Easy", 1, revision);
    invalid = note(4, 1); invalid.protectedReason = "受保护";
    reject({invalid}, "Easy", 1, revision);
    reject({note(4, 1)}, "Easy", 9, revision);
    reject({note(4, 1)}, "../outside", 1, revision);
    reject({note(4, 1)}, "Easy", 1, revision - 1);
    QVERIFY(doc.applyGeneratedChart({note(4, 1)}, "Easy", 1, revision, &error));
    QVERIFY(doc.undo());
    QCOMPARE(allChartSignatures(doc), before);
    QCOMPARE(doc.currentDifficultyId(), selected);
    QVERIFY(!doc.isModified());
}

void GeneratedChartTest::allDifficultyExportsAndProjectRecovery() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "保存", 123, 0.37, {}, &error));
    const QStringList names{"Easy", "Normal", "Hard", "Expert", "ExpertPlus"};
    const QVector<int> ranks{1, 3, 5, 7, 9};
    const QString project = root.filePath("saved/project.lmsc");
    QVERIFY(doc.saveProject(project, &error));
    QJsonObject originalManifest;
    QVERIFY(ProjectStore::readJson(project, &originalManifest, &error));
    const QString assets = QDir(QFileInfo(project).absolutePath()).filePath(originalManifest.value("assets").toString());
    const auto originalBytes = assetBytes(assets);
    QVERIFY(!originalBytes.isEmpty());
    QMap<QString, QString> generated;
    for (int i = 0; i < names.size(); ++i) {
        QVERIFY2(doc.applyGeneratedChart({note(4 + i, 1)}, names[i], ranks[i], doc.revision(), &error), qPrintable(error));
        QCOMPARE(doc.difficulties().size(), i + 1);
        generated.insert(names[i], chartSignature(doc));
        QCOMPARE(allChartSignatures(doc), generated);
        QVERIFY2(doc.saveProject(project, &error), qPrintable(error));
        QVERIFY(!doc.isModified());
        QCOMPARE(assetBytes(assets), originalBytes);
        const QString exported = root.filePath("export-" + names[i]);
        QVERIFY2(doc.exportSong(exported, &error), qPrintable(error));
        QJsonObject exportedInfo;
        QVERIFY(ProjectStore::readJson(QDir(exported).filePath("Info.dat"), &exportedInfo, &error));
        const auto maps = exportedInfo.value("_difficultyBeatmapSets").toArray().first().toObject()
            .value("_difficultyBeatmaps").toArray();
        QCOMPARE(maps.size(), i + 1);
        QSet<QString> files;
        for (int j = 0; j <= i; ++j) {
            QJsonObject map;
            QVERIFY(ProjectStore::readJson(QDir(exported).filePath(names[j] + ".dat"), &map, &error));
            QCOMPARE(map.value("_notes").toArray().size(), 1);
            QCOMPARE(map.value("_notes").toArray().first().toObject().value("_time").toDouble(),
                     4.0 + j + 0.37 * 123.0 / 60.0);
            bool matched = false;
            for (const auto &entry : maps) {
                const auto descriptor = entry.toObject();
                if (descriptor.value("_difficulty").toString() != names[j]) continue;
                QCOMPARE(descriptor.value("_difficultyRank").toInt(), ranks[j]);
                QCOMPARE(descriptor.value("_beatmapFilename").toString(), names[j] + ".dat");
                QVERIFY(!files.contains(descriptor.value("_beatmapFilename").toString()));
                files.insert(descriptor.value("_beatmapFilename").toString());
                matched = true;
            }
            QVERIFY(matched);
        }
        if (i < 3) QVERIFY(!QFileInfo::exists(QDir(exported).filePath("Expert.dat")));
        QCOMPARE(assetBytes(assets), originalBytes);
        BeatmapDocument reopened;
        QVERIFY2(reopened.loadProject(project, &error), qPrintable(error));
        QVERIFY(reopened.isNewSong());
        QCOMPARE(reopened.difficulties().size(), i + 1);
        QCOMPARE(reopened.currentDifficultyId(), doc.currentDifficultyId());
        QCOMPARE(allChartSignatures(reopened), generated);
        QCOMPARE(reopened.objects().size(), 1);
        QCOMPARE(reopened.timeMap().beatToSeconds(reopened.objects().first().beat), doc.timeMap().beatToSeconds(doc.objects().first().beat));
        QVERIFY(doc.undo());
        QVERIFY(doc.isModified());
        QVERIFY(doc.redo());
        QVERIFY(!doc.isModified());
        QCOMPARE(allChartSignatures(doc), generated);
    }

    // Autosave preserves a replacement alongside every other generated chart.
    QVERIFY(doc.applyGeneratedChart({note(12, 0)}, "Normal", 3, doc.revision(), &error));
    const auto recoveredCharts = allChartSignatures(doc);
    QVERIFY(doc.autoSave(&error));
    QVERIFY(BeatmapDocument::hasRecovery(project));
    BeatmapDocument recovered;
    QVERIFY2(recovered.loadProject(BeatmapDocument::recoveryPath(project), &error), qPrintable(error));
    QCOMPARE(recovered.difficulties().size(), 5);
    QCOMPARE(recovered.currentDifficultyId(), doc.currentDifficultyId());
    QCOMPARE(allChartSignatures(recovered), recoveredCharts);
    QVERIFY(recovered.isModified());
    BeatmapDocument saved;
    QVERIFY(saved.loadProject(project, &error));
    QCOMPARE(allChartSignatures(saved), generated);
    QCOMPARE(assetBytes(assets), originalBytes);
    QVERIFY(doc.saveProject(project, &error));
    QVERIFY(!BeatmapDocument::hasRecovery(project));
    BeatmapDocument resaved;
    QVERIFY(resaved.loadProject(project, &error));
    QCOMPARE(allChartSignatures(resaved), recoveredCharts);
    QCOMPARE(assetBytes(assets), originalBytes);
}

void GeneratedChartTest::autosaveRecoversNewDifficultyAfterSingleChartSave() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "新增谱恢复", 120, 0.25, {}, &error));
    QVERIFY(doc.applyGeneratedChart({note(2, 0), note(4, 3, 1)}, "Easy", 1, doc.revision(), &error));
    const QString easyId = doc.currentDifficultyId();
    const QString easy = chartSignature(doc);
    const QString project = root.filePath("saved/project.lmsc");
    QVERIFY(doc.saveProject(project, &error));
    QJsonObject manifest;
    QVERIFY(ProjectStore::readJson(project, &manifest, &error));
    const QString assets = QDir(QFileInfo(project).absolutePath()).filePath(manifest.value("assets").toString());
    const auto originalBytes = assetBytes(assets);
    QVERIFY(!originalBytes.isEmpty());

    QVERIFY(doc.applyGeneratedChart({note(8, 1), note(10, 2, 1)}, "Hard", 5, doc.revision(), &error));
    const QString hardId = doc.currentDifficultyId();
    const auto after = allChartSignatures(doc);
    QCOMPARE(after.value("Easy"), easy);
    QVERIFY(doc.autoSave(&error));
    QVERIFY(BeatmapDocument::hasRecovery(project));
    QCOMPARE(assetBytes(assets), originalBytes);
    BeatmapDocument saved;
    QVERIFY(saved.loadProject(project, &error));
    QCOMPARE(saved.difficulties().size(), 1);
    QCOMPARE(saved.currentDifficultyId(), easyId);
    QCOMPARE(chartSignature(saved), easy);
    BeatmapDocument recovered;
    QVERIFY2(recovered.loadProject(BeatmapDocument::recoveryPath(project), &error), qPrintable(error));
    QCOMPARE(recovered.difficulties().size(), 2);
    QCOMPARE(recovered.currentDifficultyId(), hardId);
    QCOMPARE(allChartSignatures(recovered), after);
    QVERIFY(recovered.isModified());
    QCOMPARE(assetBytes(assets), originalBytes);
    QVERIFY(recovered.saveProject(project, &error));
    BeatmapDocument resaved;
    QVERIFY2(resaved.loadProject(project, &error), qPrintable(error));
    QCOMPARE(resaved.difficulties().size(), 2);
    QCOMPARE(allChartSignatures(resaved), after);
    QVERIFY(!resaved.isModified());
    QCOMPARE(assetBytes(assets), originalBytes);
}

void GeneratedChartTest::corruptedMultiDifficultyProjectsAreRejectedAtomically() {
    QTemporaryDir root;
    BeatmapDocument source;
    QString error;
    QVERIFY(source.createNew(audioFixture(root), "恢复安全", 120, 0.25, {}, &error));
    QVERIFY(source.applyGeneratedChart({note(2, 0)}, "Easy", 1, source.revision(), &error));
    QVERIFY(source.applyGeneratedChart({note(6, 3, 1)}, "Hard", 5, source.revision(), &error));
    const QString project = root.filePath("saved/project.lmsc");
    QVERIFY(source.saveProject(project, &error));
    QJsonObject valid;
    QVERIFY(ProjectStore::readJson(project, &valid, &error));
    const auto validRows = valid.value("difficulties").toArray();
    QCOMPARE(validRows.size(), 2);
    QVERIFY(!validRows.first().toObject().value("generated").toBool());
    QVERIFY(validRows.at(1).toObject().value("generated").toBool());
    const QString assets = QDir(QFileInfo(project).absolutePath()).filePath(valid.value("assets").toString());
    const auto originalBytes = assetBytes(assets);
    QVERIFY(!originalBytes.isEmpty());

    BeatmapDocument alreadyLoaded;
    QVERIFY(alreadyLoaded.loadProject(project, &error));
    const auto before = allChartSignatures(alreadyLoaded);
    const QString selected = alreadyLoaded.currentDifficultyId();
    const QString title = alreadyLoaded.title();
    const QString loadedProject = alreadyLoaded.projectPath();
    const bool modified = alreadyLoaded.isModified();
    const bool canUndo = alreadyLoaded.canUndo();
    const bool canRedo = alreadyLoaded.canRedo();

    auto changeRow = [](QJsonObject &state, int index, const QString &key, const QJsonValue &value) {
        auto rows = state.value("difficulties").toArray();
        auto row = rows.at(index).toObject();
        row.insert(key, value);
        rows.replace(index, row);
        state.insert("difficulties", rows);
    };
    auto changeMaps = [](QJsonObject &state, const std::function<void(QJsonArray &)> &modify) {
        auto info = state.value("info").toObject();
        auto sets = info.value("_difficultyBeatmapSets").toArray();
        auto set = sets.first().toObject();
        auto maps = set.value("_difficultyBeatmaps").toArray();
        modify(maps);
        set.insert("_difficultyBeatmaps", maps);
        sets.replace(0, set);
        info.insert("_difficultyBeatmapSets", sets);
        state.insert("info", info);
    };
    auto changeGeneratedEdit = [&](QJsonObject &state, const QString &key, const QJsonValue &value) {
        auto rows = state.value("difficulties").toArray();
        auto row = rows.at(1).toObject();
        auto edits = row.value("edits").toArray();
        auto edit = edits.first().toObject();
        edit.insert(key, value);
        edits.replace(0, edit);
        row.insert("edits", edits);
        rows.replace(1, row);
        state.insert("difficulties", rows);
    };
    using Mutation = std::function<void(QJsonObject &)>;
    const QVector<QPair<QString, Mutation>> cases{
        {"invalid generated UUID", [&](QJsonObject &state) { changeRow(state, 1, "id", "generated:invalid"); }},
        {"noncanonical generated UUID", [&](QJsonObject &state) {
            const QString uuid = validRows.at(1).toObject().value("id").toString().mid(QStringLiteral("generated:").size());
            changeRow(state, 1, "id", "generated:{" + uuid + '}');
        }},
        {"attempt to replace audio asset", [&](QJsonObject &state) { changeRow(state, 1, "file", "song.ogg"); }},
        {"attempt to replace original chart asset", [&](QJsonObject &state) { changeRow(state, 1, "file", "Expert.dat"); }},
        {"generated marker removed", [&](QJsonObject &state) { changeRow(state, 1, "generated", false); }},
        {"base track marked generated", [&](QJsonObject &state) { changeRow(state, 0, "generated", true); }},
        {"duplicate difficulty ID", [&](QJsonObject &state) { changeRow(state, 1, "id", validRows.first().toObject().value("id")); }},
        {"duplicate difficulty file", [&](QJsonObject &state) { changeRow(state, 1, "file", validRows.first().toObject().value("file")); }},
        {"missing Info mapping", [&](QJsonObject &state) {
            changeMaps(state, [](QJsonArray &maps) {
                auto map = maps.at(1).toObject(); map.insert("_beatmapFilename", "missing.dat"); maps.replace(1, map);
            });
        }},
        {"duplicate Info mapping", [&](QJsonObject &state) {
            changeMaps(state, [](QJsonArray &maps) { maps.replace(0, maps.at(1)); });
        }},
        {"base record removed", [&](QJsonObject &state) {
            auto rows = state.value("difficulties").toArray(); rows.removeAt(0); state.insert("difficulties", rows);
            changeMaps(state, [](QJsonArray &maps) { maps.removeAt(0); });
        }},
        {"forged source index", [&](QJsonObject &state) { changeGeneratedEdit(state, "index", 0); }},
        {"negative source index", [&](QJsonObject &state) { changeGeneratedEdit(state, "index", -2); }},
        {"extended protected lane", [&](QJsonObject &state) { changeGeneratedEdit(state, "x", 4); }},
        {"extended protected layer", [&](QJsonObject &state) { changeGeneratedEdit(state, "y", 3); }},
        {"invalid direction", [&](QJsonObject &state) { changeGeneratedEdit(state, "direction", 9); }},
        {"wrong source array", [&](QJsonObject &state) { changeGeneratedEdit(state, "array", "colorNotes"); }},
        {"duplicate object ID", [&](QJsonObject &state) {
            auto rows = state.value("difficulties").toArray(); auto row = rows.at(1).toObject();
            auto edits = row.value("edits").toArray(); edits.append(edits.first());
            row.insert("edits", edits); rows.replace(1, row); state.insert("difficulties", rows);
        }}
    };
    int caseIndex = 0;
    for (const auto &testCase : cases) {
        QJsonObject corrupted = valid;
        testCase.second(corrupted);
        const QString tamperedFile = root.filePath(QStringLiteral("saved/tampered-%1.lmsc").arg(caseIndex++));
        QVERIFY(ProjectStore::writeJson(tamperedFile, corrupted, &error));
        const quint64 revision = alreadyLoaded.revision();
        QVERIFY2(!alreadyLoaded.loadProject(tamperedFile, &error), qPrintable(testCase.first));
        QVERIFY2(!error.isEmpty(), qPrintable(testCase.first));
        QCOMPARE(alreadyLoaded.revision(), revision);
        QCOMPARE(alreadyLoaded.currentDifficultyId(), selected);
        QCOMPARE(allChartSignatures(alreadyLoaded), before);
        QCOMPARE(alreadyLoaded.title(), title);
        QCOMPARE(alreadyLoaded.projectPath(), loadedProject);
        QCOMPARE(alreadyLoaded.isModified(), modified);
        QCOMPARE(alreadyLoaded.canUndo(), canUndo);
        QCOMPARE(alreadyLoaded.canRedo(), canRedo);
        QCOMPARE(assetBytes(assets), originalBytes);
    }
}

void GeneratedChartTest::legacySingleDifficultyProjectsRemainEditable() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "旧单谱工程", 120, 0.2, {}, &error));
    QVERIFY(doc.setNewSongDifficulty("Normal", 3, &error));
    QVERIFY(doc.addObject(note(4, 0), &error));
    const QString project = root.filePath("legacy/project.lmsc");
    QVERIFY(doc.saveProject(project, &error));
    QJsonObject current;
    QVERIFY(ProjectStore::readJson(project, &current, &error));
    // Recreate the old v1 state: track identity and edits, without any new-track metadata.
    const auto track = current.value("difficulties").toArray().first().toObject();
    QJsonObject legacy;
    for (const auto &key : QStringList{"format", "version", "newSong", "selectedDifficulty", "info",
                                      "firstBeatSeconds", "assetHashes", "source", "assets", "checkpoint"})
        legacy.insert(key, current.value(key));
    legacy.insert("difficulties", QJsonArray{QJsonObject{{"id", track.value("id")},
        {"file", track.value("file")}, {"edits", track.value("edits")}}});
    QVERIFY(ProjectStore::writeJson(project, legacy, &error));
    BeatmapDocument reopened;
    QVERIFY2(reopened.loadProject(project, &error), qPrintable(error));
    QCOMPARE(reopened.difficulties().size(), 1);
    QCOMPARE(reopened.difficulties().first().name, QString("Normal"));
    QCOMPARE(reopened.objects().first().beat, 4.0);
    QCOMPARE(reopened.timeMap().firstBeatSeconds(), 0.2);
    const QString normal = chartSignature(reopened);
    QVERIFY(reopened.applyGeneratedChart({note(8, 3, 1)}, "Easy", 1, reopened.revision(), &error));
    QCOMPARE(reopened.difficulties().size(), 2);
    QCOMPARE(allChartSignatures(reopened).value("Normal"), normal);
    QVERIFY(reopened.saveProject(project, &error));
    BeatmapDocument upgraded;
    QVERIFY2(upgraded.loadProject(project, &error), qPrintable(error));
    QCOMPARE(allChartSignatures(upgraded), allChartSignatures(reopened));
}

void GeneratedChartTest::legacyDifficultyCaseMatchesGeneratedTarget() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "旧难度名称大小写", 120, 0, {}, &error));
    QVERIFY(doc.setNewSongDifficulty("easy", 1, &error));
    QVERIFY(doc.addObject(note(2, 0), &error));
    const QString project = root.filePath("legacy-case/project.lmsc");
    QVERIFY(doc.saveProject(project, &error));
    BeatmapDocument reopened;
    QVERIFY2(reopened.loadProject(project, &error), qPrintable(error));
    const QString originalId = reopened.currentDifficultyId();
    const QString before = chartSignature(reopened);
    QCOMPARE(reopened.difficulties().first().name, QString("easy"));
    QVERIFY2(reopened.applyGeneratedChart({note(8, 3, 1)}, "Easy", 1, reopened.revision(), &error), qPrintable(error));
    QCOMPARE(reopened.difficulties().size(), 1);
    QCOMPARE(reopened.currentDifficultyId(), originalId);
    QCOMPARE(reopened.difficulties().first().name, QString("Easy"));
    QCOMPARE(reopened.objects().size(), 1);
    QCOMPARE(reopened.objects().first().beat, 8.0);
    const QString generated = chartSignature(reopened);
    QVERIFY(reopened.saveProject(project, &error));
    BeatmapDocument resaved;
    QVERIFY2(resaved.loadProject(project, &error), qPrintable(error));
    QCOMPARE(resaved.difficulties().size(), 1);
    QCOMPARE(resaved.currentDifficultyId(), originalId);
    QCOMPARE(chartSignature(resaved), generated);
    QVERIFY(reopened.undo());
    QCOMPARE(reopened.difficulties().size(), 1);
    QCOMPARE(reopened.difficulties().first().name, QString("easy"));
    QCOMPARE(chartSignature(reopened), before);
    QVERIFY(reopened.redo());
    QCOMPARE(chartSignature(reopened), generated);
}

void GeneratedChartTest::generatedExportDoesNotOverwriteUnreferencedAsset() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "保留额外文件", 120, 0, {}, &error));
    QVERIFY(doc.applyGeneratedChart({note(2, 0)}, "Easy", 1, doc.revision(), &error));
    const QString project = root.filePath("extra-asset/project.lmsc");
    QVERIFY(doc.saveProject(project, &error));
    QJsonObject manifest;
    QVERIFY(ProjectStore::readJson(project, &manifest, &error));
    const QString assets = QDir(QFileInfo(project).absolutePath()).filePath(manifest.value("assets").toString());
    const QByteArray extraBytes("Unreferenced user resource: preserve these bytes");
    QFile extra(QDir(assets).filePath("Hard.dat"));
    QVERIFY(extra.open(QIODevice::WriteOnly));
    QCOMPARE(extra.write(extraBytes), qint64(extraBytes.size()));
    extra.close();
    QJsonObject hashes;
    const auto originalBytes = assetBytes(assets);
    for (auto it = originalBytes.cbegin(); it != originalBytes.cend(); ++it)
        hashes.insert(it.key(), QString::fromLatin1(it.value().toHex()));
    manifest.insert("assetHashes", hashes);
    QVERIFY(ProjectStore::writeJson(project, manifest, &error));

    BeatmapDocument reopened;
    QVERIFY2(reopened.loadProject(project, &error), qPrintable(error));
    const QString easy = chartSignature(reopened);
    QVERIFY2(reopened.applyGeneratedChart({note(6, 3, 1)}, "Hard", 5, reopened.revision(), &error), qPrintable(error));
    QCOMPARE(reopened.difficulties().size(), 2);
    QCOMPARE(allChartSignatures(reopened).value("Easy"), easy);
    const auto beforeExport = allChartSignatures(reopened);
    const QString selected = reopened.currentDifficultyId();
    const quint64 revision = reopened.revision();
    const QString output = root.filePath("blocked-export");
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(!reopened.exportSong(output, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(output));
    QCOMPARE(reopened.revision(), revision);
    QCOMPARE(reopened.currentDifficultyId(), selected);
    QCOMPARE(allChartSignatures(reopened), beforeExport);
    QCOMPARE(assetBytes(assets), originalBytes);
    QVERIFY(extra.open(QIODevice::ReadOnly));
    QCOMPARE(extra.readAll(), extraBytes);
}

void GeneratedChartTest::importedChartsCannotBeReplaced() {
    QTemporaryDir root;
    BeatmapDocument original;
    QString error;
    QVERIFY(original.createNew(audioFixture(root), "导入", 120, 0, {}, &error));
    QVERIFY(original.applyGeneratedChart({note(3, 1)}, "Easy", 1, original.revision(), &error));
    QVERIFY(original.applyGeneratedChart({note(6, 2, 1)}, "Hard", 5, original.revision(), &error));
    const QString exported = root.filePath("song");
    QVERIFY(original.exportSong(exported, &error));
    BeatmapDocument imported;
    QVERIFY(imported.loadSong(exported, &error));
    const auto before = allChartSignatures(imported);
    const auto revision = imported.revision();
    QVERIFY(!imported.applyGeneratedChart({note(4, 0)}, "Easy", 1, revision, &error));
    QCOMPARE(imported.revision(), revision);
    QCOMPARE(allChartSignatures(imported), before);

    // Imported song editing retains the original per-track undo contract.
    const QString easyId = difficultyId(imported, "Easy");
    const QString hardId = difficultyId(imported, "Hard");
    QVERIFY(imported.setDifficulty(easyId, &error));
    QVERIFY(imported.addObject(note(10, 0), &error));
    QVERIFY(imported.setDifficulty(hardId, &error));
    QVERIFY(imported.addObject(note(12, 3, 1), &error));
    const QString editedHard = chartSignature(imported);
    QVERIFY(imported.setDifficulty(easyId, &error));
    QVERIFY(imported.undo());
    QCOMPARE(chartSignature(imported), before.value("Easy"));
    QCOMPARE(allChartSignatures(imported).value("Hard"), editedHard);
    QVERIFY(imported.redo());
    QCOMPARE(imported.objects().size(), 2);
}

void GeneratedChartTest::revisionsNeverReturnToSavedValues() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "修订", 120, 0, {}, &error));
    quint64 previous = doc.revision();
    QVERIFY(doc.addObject(note(4, 0), &error)); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.undo()); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.redo()); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.setNewSongDifficulty("Normal", 3, &error)); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.undo()); QVERIFY(doc.revision() > previous); QCOMPARE(doc.difficulties().first().name, QString("Expert")); previous = doc.revision();
    QVERIFY(doc.setNewSongTempo(150, 0.5, &error)); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.createNew(root.filePath("source.ogg"), "另首", 120, 0, {}, &error));
    QVERIFY(doc.revision() > previous);
}

QTEST_GUILESS_MAIN(GeneratedChartTest)
#include "GeneratedChartTest.moc"
