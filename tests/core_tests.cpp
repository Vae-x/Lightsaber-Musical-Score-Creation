#include "core/BeatmapDocument.h"
#include "core/ProjectStore.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>
#include <stdexcept>

using namespace lmsc;

namespace {
void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toUtf8().constData());
}
QJsonObject info(const QString &file = "Expert.dat") {
    QJsonObject difficulty{{"_difficulty", "Expert"}, {"_difficultyRank", 7}, {"_beatmapFilename", file}};
    return {{"_version", "2.0.0"}, {"_songName", "Test"}, {"_beatsPerMinute", 120},
            {"_songTimeOffset", 0}, {"_songFilename", "song.ogg"}, {"_coverImageFilename", ""},
            {"_unknown", QJsonObject{{"keep", true}}},
            {"_difficultyBeatmapSets", QJsonArray{QJsonObject{{"_beatmapCharacteristicName", "Standard"},
                                                           {"_difficultyBeatmaps", QJsonArray{difficulty}}}}}};
}
void song(const QString &folder, const QJsonObject &map, bool eggReference = false) {
    require(QDir().mkpath(folder), "create test song");
    QString error;
    auto metadata = info();
    if (eggReference) metadata.insert("_songFilename", "song.egg");
    require(ProjectStore::writeJson(QDir(folder).filePath("Info.dat"), metadata, &error), error);
    require(ProjectStore::writeJson(QDir(folder).filePath("Expert.dat"), map, &error), error);
    QFile audio(QDir(folder).filePath("song.ogg"));
    require(audio.open(QIODevice::WriteOnly), "create test audio");
    audio.write("OggS test fixture: bytes must remain identical");
}
QByteArray hash(const QString &file) {
    QFile input(file);
    require(input.open(QIODevice::ReadOnly), "hash input");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    require(digest.addData(&input), "hash file");
    return digest.result();
}
void compareFolders(const QString &a, const QString &b) {
    const auto af = ProjectStore::files(a), bf = ProjectStore::files(b);
    require(af == bf, "roundtrip file list mismatch");
    for (const auto &relative : af)
        require(hash(QDir(a).filePath(relative)) == hash(QDir(b).filePath(relative)), "roundtrip bytes differ: " + relative);
}
QJsonObject map3() {
    return {{"version", "3.3.0"}, {"bpmEvents", QJsonArray{QJsonObject{{"b", 8}, {"m", 240}}}},
            {"colorNotes", QJsonArray{QJsonObject{{"b", 1}, {"x", 0}, {"y", 0}, {"c", 0}, {"d", 1}, {"a", 0}, {"unknown", 29}},
                                        QJsonObject{{"b", 2}, {"x", 1}, {"y", 1}, {"c", 1}, {"d", 0}, {"a", 0}},
                                        QJsonObject{{"b", 4}, {"x", 2}, {"y", 2}, {"c", 1}, {"d", 0}, {"a", 0},
                                                    {"customData", QJsonObject{{"track", "animated"}}}}}},
            {"bombNotes", QJsonArray{}}, {"obstacles", QJsonArray{}},
            {"sliders", QJsonArray{QJsonObject{{"b", 2}, {"x", 1}, {"y", 1}, {"c", 1}, {"tb", 3}, {"tx", 2}, {"ty", 1}}}},
            {"burstSliders", QJsonArray{}}, {"customData", QJsonObject{{"customEvents", QJsonArray{QJsonObject{{"type", "AnimateTrack"}}}}}},
            {"unrecognized", QJsonObject{{"nested", QJsonArray{1, 2, 3}}}}};
}
void unitTests(const QString &root) {
    TimeMap time;
    QString error;
    require(time.configure(120, 0.25, {{8, 240}, {12, 60}}, &error), error);
    require(std::abs(time.beatToSeconds(8) - 4.25) < 1e-10, "tempo boundary 8");
    require(std::abs(time.beatToSeconds(12) - 5.25) < 1e-10, "tempo boundary 12");
    for (double beat : {-2.0, 0.0, 3.2, 8.0, 10.3, 12.0, 100.0})
        require(std::abs(time.secondsToBeat(time.beatToSeconds(beat)) - beat) < 1e-9, "time inverse");
    require(!time.configure(120, 0, {{2, 0}}, &error), "invalid tempo rejected");
    require(!ProjectStore::safeRelativePath("../outside") && !ProjectStore::safeRelativePath("C:/outside") &&
            !ProjectStore::safeRelativePath("safe/../outside") && !ProjectStore::safeRelativePath("NUL.dat") &&
            ProjectStore::safeRelativePath("nested/song.ogg"), "path validation");

    const QString original = QDir(root).filePath("original");
    song(original, map3(), true);
    BeatmapDocument document;
    require(document.loadSong(original, &error), error);
    require(document.objects().size() == 3 && document.warnings().join(' ').contains(".egg"), "egg fallback and object count");
    require(std::abs(document.timeMap().beatToSeconds(10) - 4.5) < 1e-10, "v3 BPM events");
    const QString unchanged = QDir(root).filePath("unchanged");
    require(document.exportSong(unchanged, &error), error);
    compareFolders(original, unchanged);
    const auto baseline = document.objects();
    require(!baseline[0].isProtected() && baseline[1].isProtected() && baseline[2].isProtected(), "arc and mod protection");
    auto edit = baseline[0];
    edit.direction = 6;
    auto protectedEdit = baseline[1];
    protectedEdit.protectedReason.clear();
    protectedEdit.beat += 1;
    require(!document.updateObjects({edit, protectedEdit}, &error), "protected batch rejected");
    require(document.objects()[0].direction == baseline[0].direction && !document.canUndo(), "batch atomicity");
    require(!document.removeObjects({baseline[0].id, baseline[1].id}, &error), "protected batch delete rejected");
    require(document.objects().size() == 3, "batch delete atomicity");
    require(document.copyObjects({baseline[1].id}, &error).isEmpty(), "protected copy");
    BeatObject collision;
    collision.beat = 3; collision.x = 2; collision.y = 1;
    require(!document.addObject(collision, &error), "advanced endpoint paste conflict");
    require(document.updateObject(edit, &error), error);
    require(document.canUndo() && document.isModified(), "edit dirty");
    const QString changed = QDir(root).filePath("changed");
    require(document.exportSong(changed, &error), error);
    QJsonObject exported;
    require(ProjectStore::readJson(QDir(changed).filePath("Expert.dat"), &exported, &error), error);
    require(exported.value("colorNotes").toArray()[0].toObject().value("unknown").toInt() == 29, "unknown note field preserved");
    require(exported.value("sliders") == map3().value("sliders") && exported.value("customData") == map3().value("customData"), "advanced data preserved");
    require(document.undo() && !document.isModified(), "undo to clean");
    const QString undoExport = QDir(root).filePath("undo-export");
    require(document.exportSong(undoExport, &error), error);
    compareFolders(original, undoExport);
    require(document.redo() && document.objects()[0].direction == 6, "redo");
    const QString project = QDir(root).filePath("project/project.lmsc");
    require(document.saveProject(project, &error), error);
    require(!document.isModified(), "save clears dirty");
    BeatmapDocument reopened;
    require(reopened.loadProject(project, &error), error);
    require(reopened.objects()[0].direction == 6, "saved edit recovery");
    auto recoveryEdit = reopened.objects()[0];
    recoveryEdit.beat = 5;
    require(reopened.updateObject(recoveryEdit, &error), error);
    require(reopened.autoSave(&error) && BeatmapDocument::hasRecovery(project), error);
    BeatmapDocument recovered;
    require(recovered.loadProject(BeatmapDocument::recoveryPath(project), &error), error);
    require(recovered.isModified(), "autosave recovery dirty");
    bool beat5 = false;
    for (const auto &object : recovered.objects()) if (object.id == recoveryEdit.id && object.beat == 5) beat5 = true;
    require(beat5, "autosave recovery object");
    recoveryEdit.beat = 6;
    require(reopened.updateObject(recoveryEdit, &error) && reopened.saveProject(project, &error) &&
            !BeatmapDocument::hasRecovery(project), "stale recovery checkpoint detection");
    BeatmapDocument stale;
    require(!stale.loadProject(BeatmapDocument::recoveryPath(project), &error), "stale recovery cannot revert later save");
    require(!reopened.exportSong(changed, &error), "export refuses existing folder");

    // Actual legacy type-100 BPM changes use _floatValue, never integer _value.
    const QString v2folder = QDir(root).filePath("v2");
    song(v2folder, {{"_version", "2.5.0"}, {"_notes", QJsonArray{QJsonObject{{"_time", 1}, {"_lineIndex", 0}, {"_lineLayer", 0}, {"_type", 0}, {"_cutDirection", 1}}}},
                    {"_obstacles", QJsonArray{}}, {"_events", QJsonArray{QJsonObject{{"_time", 4}, {"_type", 100}, {"_value", 0}, {"_floatValue", 60}}}}});
    BeatmapDocument legacy;
    require(legacy.loadSong(v2folder, &error), error);
    require(std::abs(legacy.timeMap().beatToSeconds(8) - 6) < 1e-10, "v2 BPM conversion");
    auto legacyObject = legacy.objects()[0]; legacyObject.beat = 2;
    require(legacy.updateObject(legacyObject, &error), error);
    require(legacy.undo() && legacy.objects()[0].beat == 1, "v2 undo");
    const QString unknownTiming = QDir(root).filePath("unknown-timing");
    auto timedMap = map3();
    timedMap.insert("customData", QJsonObject{{"bpmChanges", QJsonArray{QJsonObject{{"beat", 2}, {"bpm", 200}}}}});
    song(unknownTiming, timedMap);
    BeatmapDocument unknown;
    require(unknown.loadSong(unknownTiming, &error) && !unknown.readOnlyReason().isEmpty(), "unknown time protection");
    require(!unknown.addObject(BeatObject{}, &error), "unknown timing cannot edit");

    BeatmapDocument fresh;
    require(fresh.createNew(QDir(original).filePath("song.ogg"), "New song", 135, 0.4, {}, &error), error);
    require(fresh.setNewSongMetadata("New title", "Artist", "Mapper", &error), error);
    require(fresh.setNewSongDifficulty("Hard", 5, &error), error);
    require(fresh.setNewSongTempo(140, 0.3, &error), error);
    const QString originalMedia = QDir(root).filePath("source.mp4");
    QFile video(originalMedia);
    require(video.open(QIODevice::WriteOnly), "create source media fixture");
    video.write("source movie bytes: keep but do not export"); video.close();
    require(fresh.setImportSource(originalMedia, 2, 1.5, 15.25, &error), error);
    BeatObject newNote; newNote.beat = 4; newNote.x = 0; newNote.y = 1; newNote.direction = 4;
    require(fresh.addObject(newNote, &error), error);
    const auto inserted = fresh.objects()[0];
    require(fresh.mirrorObjects({inserted.id}, &error), error);
    require(fresh.objects()[0].x == 3 && fresh.objects()[0].color == 1 && fresh.objects()[0].direction == 5, "mirror all properties");
    require(fresh.undo() && fresh.objects()[0].x == 0, "mirror undo");
    require(fresh.pasteObjects(fresh.copyObjects({inserted.id}, &error), 4, 1, false, &error), error);
    require(fresh.objects().size() == 2 && fresh.undo() && fresh.objects().size() == 1, "paste undo");
    const QString newProject = QDir(root).filePath("new-project");
    require(fresh.saveProject(newProject, &error), error);
    BeatmapDocument newReopened;
    require(newReopened.loadProject(newProject, &error), error);
    require(newReopened.isNewSong() && newReopened.title() == "New title" && newReopened.difficulties()[0].name == "Hard" &&
            newReopened.timeMap().baseBpm() == 140 && std::abs(newReopened.timeMap().firstBeatSeconds() - .3) < 1e-10, "new project metadata roundtrip");
    require(newReopened.objects().size() == 1, "new project objects");
    const auto source = newReopened.importSource();
    require(source.isAvailable() && source.streamIndex == 2 && source.startSeconds == 1.5 && source.endSeconds == 15.25 &&
            hash(source.path) == hash(originalMedia), "original import source snapshot");
    const QString newExport = QDir(root).filePath("new-export");
    require(newReopened.exportSong(newExport, &error), error);
    require(!ProjectStore::files(newExport).join(' ').contains("original.mp4"), "source media excluded from exported game song");
    QJsonObject newMap, newInfo;
    require(ProjectStore::readJson(QDir(newExport).filePath("Hard.dat"), &newMap, &error) &&
            ProjectStore::readJson(QDir(newExport).filePath("Info.dat"), &newInfo, &error), error);
    require(!QFileInfo::exists(QDir(newExport).filePath("Expert.dat")) &&
            newInfo.value("_difficultyBeatmapSets").toArray().first().toObject()
                .value("_difficultyBeatmaps").toArray().first().toObject().value("_beatmapFilename").toString() == "Hard.dat",
            "new-song export uses the selected difficulty filename without a stale Expert copy");
    require(newMap.value("_version").toString() == "2.2.0" && newInfo.value("_songTimeOffset").toDouble() == 0.0 &&
            std::abs(newMap.value("_notes").toArray().first().toObject().value("_time").toDouble() - 4.7) < 1e-10,
            "first-beat alignment baked into exported beat times, deprecated offset zero");
    BeatmapDocument exportedReopened;
    require(exportedReopened.loadSong(newExport, &error) &&
            std::abs(exportedReopened.timeMap().beatToSeconds(exportedReopened.objects()[0].beat) - fresh.timeMap().beatToSeconds(4)) < 1e-10,
            "exported and editor audio time agree");
    auto secondNote = newReopened.objects()[0]; secondNote.beat = 10;
    require(newReopened.updateObject(secondNote, &error) && newReopened.autoSave(&error), error);
    BeatmapDocument sourceRecovery;
    require(sourceRecovery.loadProject(BeatmapDocument::recoveryPath(newProject), &error) && sourceRecovery.importSource().isAvailable(), "source recovery snapshot");
    const QStringList difficultyNames{"Easy", "Normal", "Hard", "Expert", "ExpertPlus"};
    const QVector<int> difficultyRanks{1, 3, 5, 7, 9};
    QJsonObject snapshot;
    require(ProjectStore::readJson(QDir(newProject).filePath("project.lmsc"), &snapshot, &error), error);
    const QString snapshotAssets = QDir(newProject).filePath(snapshot.value("assets").toString());
    const auto originalAssetHashes = snapshot.value("assetHashes");
    const auto snapshotFiles = ProjectStore::files(snapshotAssets);
    QHash<QString, QByteArray> snapshotBytes;
    for (const auto &relative : snapshotFiles) snapshotBytes.insert(relative, hash(QDir(snapshotAssets).filePath(relative)));
    for (int i = 0; i < difficultyNames.size(); ++i) {
        const auto &name = difficultyNames[i];
        // Changing an existing new-song project must preserve its original
        // Expert.dat snapshot and identifiers for save/recovery compatibility.
        require(newReopened.setNewSongDifficulty(name, difficultyRanks[i], &error), error);
        const auto beforeObjects = newReopened.objects();
        const auto beforeDifficulty = newReopened.difficulties().first();
        const QString output = QDir(root).filePath("difficulty-" + name);
        require(newReopened.exportSong(output, &error), error);
        QJsonObject exportedInfo, exportedMap;
        require(ProjectStore::readJson(QDir(output).filePath("Info.dat"), &exportedInfo, &error) &&
                ProjectStore::readJson(QDir(output).filePath(name + ".dat"), &exportedMap, &error), error);
        const auto exportedDifficulty = exportedInfo.value("_difficultyBeatmapSets").toArray().first().toObject()
                .value("_difficultyBeatmaps").toArray().first().toObject();
        require(exportedDifficulty.value("_difficulty").toString() == name &&
                exportedDifficulty.value("_difficultyRank").toInt() == difficultyRanks[i] &&
                exportedDifficulty.value("_beatmapFilename").toString() == name + ".dat" &&
                (name == "Expert" || !QFileInfo::exists(QDir(output).filePath("Expert.dat"))) &&
                exportedMap.value("_notes").toArray().size() == 1, "all five difficulty exports have matching metadata and map files");
        BeatmapDocument reimported;
        require(reimported.loadSong(output, &error) && reimported.difficulties().first().name == name &&
                reimported.objects().size() == beforeObjects.size(), "renamed difficulty output can be reimported");
        require(newReopened.difficulties().first().id == beforeDifficulty.id &&
                newReopened.difficulties().first().filename == "Expert.dat" &&
                newReopened.objects().first().id == beforeObjects.first().id &&
                newReopened.objects().first().beat == beforeObjects.first().beat,
                "export filename does not mutate in-memory project identity or objects");
        require(ProjectStore::files(snapshotAssets) == snapshotFiles, "export does not rename original snapshot files");
        for (const auto &relative : snapshotFiles)
            require(hash(QDir(snapshotAssets).filePath(relative)) == snapshotBytes.value(relative), "export does not modify original snapshot bytes");
    }
    require(newReopened.saveProject(newProject, &error), error);
    QJsonObject resaved;
    require(ProjectStore::readJson(QDir(newProject).filePath("project.lmsc"), &resaved, &error) &&
            resaved.value("assetHashes") == originalAssetHashes &&
            resaved.value("difficulties").toArray().first().toObject().value("file").toString() == "Expert.dat",
            "resaving a renamed export preserves legacy snapshot format");
    BeatmapDocument renamedReopened;
    require(renamedReopened.loadProject(newProject, &error) && renamedReopened.difficulties().first().name == "ExpertPlus" &&
            renamedReopened.difficulties().first().filename == "Expert.dat" && renamedReopened.objects().first().beat == 10,
            "project reload survives exporting and switching all difficulties");
    BeatmapDocument emptyNew;
    require(emptyNew.createNew(QDir(original).filePath("song.ogg"), "Empty song", 120, 0, {}, &error) &&
            emptyNew.setNewSongDifficulty("Easy", 1, &error), error);
    const QString emptyExport = QDir(root).filePath("empty-easy-export");
    require(emptyNew.exportSong(emptyExport, &error) && QFileInfo::exists(QDir(emptyExport).filePath("Easy.dat")) &&
            !QFileInfo::exists(QDir(emptyExport).filePath("Expert.dat")), "unedited new-song export also renames the snapshot copy");
    require(emptyNew.setNewSongDifficulty("../outside", 1, &error), error);
    const QString unknownExport = QDir(root).filePath("unknown-difficulty-export");
    require(emptyNew.exportSong(unknownExport, &error) && QFileInfo::exists(QDir(unknownExport).filePath("Expert.dat")) &&
            !QFileInfo::exists(QDir(root).filePath("outside.dat")), "unknown difficulty names never become export paths");
    QJsonObject saved;
    require(ProjectStore::readJson(QDir(newProject).filePath("project.lmsc"), &saved, &error), error);
    QFile corrupt(QDir(newProject).filePath(saved.value("assets").toString() + "/song.ogg"));
    require(corrupt.open(QIODevice::Append), "open corrupt test copy"); corrupt.write("changed"); corrupt.close();
    BeatmapDocument corruptLoad;
    require(!corruptLoad.loadProject(newProject, &error), "original asset integrity");

#ifdef Q_OS_WIN
    // Build a hostile ZIP using only the Windows runtime available to users.
    const QString zipPath = QDir(root).filePath("escape.zip");
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LMSC_TEST_ZIP", zipPath);
    process.setProcessEnvironment(environment);
    const QString script = "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression; Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                           "$z=[System.IO.Compression.ZipFile]::Open($env:LMSC_TEST_ZIP,[System.IO.Compression.ZipArchiveMode]::Create); "
                           "$e=$z.CreateEntry('../escaped.txt'); $s=$e.Open(); $s.WriteByte(42); $s.Dispose(); $z.Dispose()";
    process.start(QDir(environment.value("SystemRoot")).filePath("System32/WindowsPowerShell/v1.0/powershell.exe"),
                  {"-NoProfile", "-NonInteractive", "-Command", script});
    require(process.waitForFinished(10000) && process.exitCode() == 0,
            "hostile zip fixture creation: " + QString::fromUtf8(process.readAllStandardError()));
    require(!ProjectStore::extractZip(zipPath, QDir(root).filePath("escape-target"), &error), "zip traversal rejected");
    require(!QFileInfo::exists(QDir(root).filePath("escaped.txt")), "zip cannot write outside destination");
#endif
}

void realSamples(const QString &sampleRoot, const QString &root) {
    const auto archives = QDir(sampleRoot).entryList({"*.zip"}, QDir::Files, QDir::Name);
    require(archives.size() == 10, "expected ten local ZIP validation samples");
    int charts = 0;
    for (int i = 0; i < archives.size(); ++i) {
        QString error;
        const QString extracted = QDir(root).filePath("sample-original-" + QString::number(i));
        require(ProjectStore::extractZip(QDir(sampleRoot).filePath(archives[i]), extracted, &error), error);
        BeatmapDocument document;
        require(document.loadZip(QDir(sampleRoot).filePath(archives[i]), &error), archives[i] + ": " + error);
        for (const auto &difficulty : document.difficulties()) {
            require(document.setDifficulty(difficulty.id, &error), error);
            ++charts;
        }
        const QString out = QDir(root).filePath("sample-export-" + QString::number(i));
        require(document.exportSong(out, &error), error);
        compareFolders(extracted, out);
        if (archives[i].startsWith("2369d")) {
            bool exercised = false;
            for (const auto &difficulty : document.difficulties()) {
                require(document.setDifficulty(difficulty.id, &error), error);
                BeatObject target;
                bool found = false;
                for (const auto &object : document.objects())
                    if (object.kind == ObjectKind::Note && !object.isProtected()) { target = object; found = true; break; }
                if (!found) continue;
                QJsonObject before;
                require(ProjectStore::readJson(QDir(extracted).filePath(difficulty.filename), &before, &error), error);
                target.direction = (target.direction + 1) % 9;
                require(document.updateObject(target, &error), error);
                const QString editedOut = QDir(root).filePath("dryhands-edited");
                require(document.exportSong(editedOut, &error), error);
                for (const auto &relative : ProjectStore::files(extracted))
                    if (relative != difficulty.filename)
                        require(hash(QDir(extracted).filePath(relative)) == hash(QDir(editedOut).filePath(relative)), "real edit changed unrelated asset " + relative);
                QJsonObject after;
                require(ProjectStore::readJson(QDir(editedOut).filePath(difficulty.filename), &after, &error), error);
                const QString array = difficulty.version.startsWith("3") ? "colorNotes" : "_notes";
                const QString directionKey = difficulty.version.startsWith("3") ? "d" : "_cutDirection";
                int countChanged = 0;
                const auto oldNotes = before.value(array).toArray(), newNotes = after.value(array).toArray();
                require(oldNotes.size() == newNotes.size(), "real edit array size");
                for (int note = 0; note < oldNotes.size(); ++note) {
                    auto oldObject = oldNotes[note].toObject(), newObject = newNotes[note].toObject();
                    if (oldObject != newObject) {
                        ++countChanged;
                        oldObject.remove(directionKey); newObject.remove(directionKey);
                        require(oldObject == newObject, "real edit changed fields beyond direction");
                    }
                }
                require(countChanged == 1, "exactly one real note field modified");
                before.remove(array); after.remove(array);
                require(before == after, "real edit changed lights, advanced arrays or unknown data");
                require(document.undo(), "real edit undo");
                const QString restoredOut = QDir(root).filePath("dryhands-undo");
                require(document.exportSong(restoredOut, &error), error);
                compareFolders(extracted, restoredOut);
                BeatObject added;
                added.beat = 1000; added.x = 0; added.y = 0;
                const int oldCount = document.objects().size();
                require(document.addObject(added, &error) && document.objects().size() == oldCount + 1 && document.undo() &&
                        document.objects().size() == oldCount, "Dry Hands new note and undo with unchanged legacy BPM marker");
                exercised = true;
                break;
            }
            require(exercised, "Dry Hands editable note fixture");
            QTextStream(stdout) << "PASS Dry Hands real note edit: one direction field only; undo byte-identical\n";
        }
        QTextStream(stdout) << "PASS raw roundtrip: " << archives[i] << "\n";
    }
    require(charts == 20, "expected twenty difficulties");
    QTextStream(stdout) << "PASS ten ZIPs / twenty difficulties / all exported file SHA256 bytes identical\n";
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary(QDir::tempPath() + "/lmsc-core-test-XXXXXX");
    try {
        require(temporary.isValid(), "test temporary directory");
        unitTests(temporary.path());
        QTextStream(stdout) << "PASS synthetic edit, preservation, protection, undo/redo, project/recovery, BPM tests\n";
        if (application.arguments().size() > 1) realSamples(application.arguments()[1], temporary.path());
        return 0;
    } catch (const std::exception &exception) {
        QTextStream(stderr) << "FAIL " << QString::fromUtf8(exception.what()) << "\n";
        return 1;
    }
}
