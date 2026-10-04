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
void editorTimingTests(const QString &root) {
    QString error;
    for (const bool useOfficialBpm : {false, true}) {
        const QString name = useOfficialBpm ? "official-bookmarks-true" : "official-bookmarks-false";
        const QString original = QDir(root).filePath(name);
        auto primary = map3(), secondary = map3();
        auto custom = primary.value("customData").toObject();
        custom.insert("bookmarksUseOfficialBpmEvents", useOfficialBpm);
        custom.insert("time", 12.5);
        custom.insert("bookmarks", QJsonArray{QJsonObject{{"b", 8}, {"n", "editor marker"}}});
        primary.insert("customData", custom);
        custom.insert("bookmarksUseOfficialBpmEvents", !useOfficialBpm);
        secondary.insert("customData", custom);
        song(original, primary);
        require(ProjectStore::writeJson(QDir(original).filePath("Hard.dat"), secondary, &error), error);
        auto metadata = info();
        auto sets = metadata.value("_difficultyBeatmapSets").toArray();
        auto set = sets[0].toObject();
        auto charts = set.value("_difficultyBeatmaps").toArray();
        charts.append(QJsonObject{{"_difficulty", "Hard"}, {"_difficultyRank", 5}, {"_beatmapFilename", "Hard.dat"}});
        set.insert("_difficultyBeatmaps", charts); sets[0] = set;
        metadata.insert("_difficultyBeatmapSets", sets);
        require(ProjectStore::writeJson(QDir(original).filePath("Info.dat"), metadata, &error), error);
        const auto originalAudio = hash(QDir(original).filePath("song.ogg"));
        const auto originalPrimary = hash(QDir(original).filePath("Expert.dat"));
        const auto originalSecondary = hash(QDir(original).filePath("Hard.dat"));
        BeatmapDocument document;
        require(document.loadSong(original, &error) && document.readOnlyReason().isEmpty(), "boolean bookmark setting is editable: " + error);
        require(std::abs(document.timeMap().beatToSeconds(10) - 4.5) < 1e-10, "bookmark setting leaves official BPM events authoritative");
        const auto baseline = document.objects();
        require(!baseline[0].isProtected() && baseline[1].isProtected() && baseline[2].isProtected(), "bookmark exemption retains arc and mod object protection");
        require(!document.mirrorObjects({baseline[0].id, baseline[1].id}, &error) && !document.canUndo() &&
                document.objects()[0].x == baseline[0].x, "mixed mirror is rejected atomically");
        auto endpointEdit = baseline[1]; endpointEdit.protectedReason.clear(); endpointEdit.direction = 8;
        require(!document.updateObject(endpointEdit, &error) && !document.removeObjects({baseline[1].id}, &error) &&
                document.copyObjects({baseline[1].id}, &error).isEmpty(), "arc endpoint remains protected through every basic edit entry");
        auto edit = baseline[0]; edit.direction = 6;
        require(document.updateObject(edit, &error) && document.undo() && !document.isModified(), "boolean setting supports basic edit and undo");
        const QString undone = QDir(root).filePath(name + "-undo-export");
        require(document.exportSong(undone, &error), error);
        compareFolders(original, undone);
        require(document.redo() && document.objects()[0].direction == 6, "boolean setting supports redo");
        const QString firstExport = QDir(root).filePath(name + "-first-export");
        require(document.exportSong(firstExport, &error), error);
        require(hash(QDir(firstExport).filePath("Hard.dat")) == originalSecondary &&
                hash(QDir(firstExport).filePath("song.ogg")) == originalAudio &&
                hash(QDir(firstExport).filePath("Info.dat")) == hash(QDir(original).filePath("Info.dat")),
                "editing primary preserves other difficulty, original audio and metadata bytes");
        const QString project = QDir(root).filePath(name + "-project/project.lmsc");
        require(document.saveProject(project, &error), error);
        BeatmapDocument reopened;
        require(reopened.loadProject(project, &error) && reopened.readOnlyReason().isEmpty() &&
                reopened.objects()[0].direction == 6, "saved bookmark setting project reopens editable");
        require(reopened.setDifficulty(reopened.difficulties()[1].id, &error) && reopened.readOnlyReason().isEmpty(),
                "opposite boolean setting in second difficulty remains editable");
        auto secondEdit = reopened.objects()[0]; secondEdit.direction = 7;
        require(reopened.updateObject(secondEdit, &error) && reopened.saveProject(project, &error), error);
        BeatmapDocument saved;
        require(saved.loadProject(project, &error) && saved.objects()[0].direction == 7, "both difficulty edits persist");
        const QString finalExport = QDir(root).filePath(name + "-final-export");
        require(saved.exportSong(finalExport, &error), error);
        for (int i = 0; i < 2; ++i) {
            const QString filename = i ? "Hard.dat" : "Expert.dat";
            const auto before = i ? secondary : primary;
            QJsonObject after;
            require(ProjectStore::readJson(QDir(finalExport).filePath(filename), &after, &error), error);
            auto expected = before;
            auto notes = expected.value("colorNotes").toArray();
            auto note = notes[0].toObject(); note.insert("d", i ? 7 : 6); notes[0] = note;
            expected.insert("colorNotes", notes);
            require(after == expected, "export changes only chosen basic note direction, preserving custom boolean, unknown JSON and advanced arrays");
        }
        require(hash(QDir(original).filePath("Expert.dat")) == originalPrimary &&
                hash(QDir(original).filePath("Hard.dat")) == originalSecondary &&
                hash(QDir(original).filePath("song.ogg")) == originalAudio &&
                hash(QDir(finalExport).filePath("song.ogg")) == originalAudio, "original maps and audio remain untouched");
    }

    const QVector<QJsonValue> invalidSettings{QStringLiteral("true"), QJsonObject{{"enabled", true}},
                                            QJsonArray{}, QJsonValue(QJsonValue::Null), 1};
    for (int i = 0; i < invalidSettings.size(); ++i) {
        const QString original = QDir(root).filePath("invalid-bookmark-setting-" + QString::number(i));
        auto map = map3();
        map.insert("customData", QJsonObject{{"bookmarksUseOfficialBpmEvents", invalidSettings[i]}});
        song(original, map);
        BeatmapDocument document;
        require(document.loadSong(original, &error), error);
        const QString reason = document.readOnlyReason();
        require(reason.contains("Expert.dat.customData.bookmarksUseOfficialBpmEvents") && reason.contains(QStringLiteral("布尔")) &&
                reason.contains(QStringLiteral("另存工程")), "invalid boolean setting reason identifies field and persistent protection");
        require(!document.updateObject(document.objects()[0], &error) && !document.addObject(BeatObject{}, &error), "invalid setting cannot edit");
        const QString project = QDir(root).filePath("invalid-bookmark-project-" + QString::number(i));
        require(document.saveProject(project, &error), error);
        BeatmapDocument reopened;
        require(reopened.loadProject(project, &error) && reopened.readOnlyReason() == reason &&
                !reopened.mirrorObjects({reopened.objects()[0].id}, &error), "saving does not unlock malformed bookmark extension");
        const QString output = QDir(root).filePath("invalid-bookmark-export-" + QString::number(i));
        require(reopened.exportSong(output, &error), error);
        compareFolders(original, output);
    }
    const QVector<QJsonObject> unknownSettings{
        QJsonObject{{"tempo", 180}}, QJsonObject{{"timeScale", 2}},
        QJsonObject{{"_BPMChanges", QJsonArray{QJsonObject{{"_time", 2}, {"_BPM", 180}}}}}};
    for (int i = 0; i < unknownSettings.size(); ++i) {
        const QString original = QDir(root).filePath("unknown-editor-timing-" + QString::number(i));
        auto map = map3(); map.insert("customData", unknownSettings[i]); song(original, map);
        BeatmapDocument document;
        require(document.loadSong(original, &error) && !document.readOnlyReason().isEmpty() &&
                document.readOnlyReason().contains("Expert.dat.customData.") &&
                !document.addObject(BeatObject{}, &error), "unknown tempo, scale and actual BPM change remain protected with field path");
    }
    auto harmless = map3();
    harmless.insert("_customData", QJsonObject{{"_BPMChanges", QJsonArray{QJsonObject{{"_time", 0}, {"_BPM", 120},
                                                      {"_beatsPerBar", 4}, {"_metronomeOffset", 0}}}}});
    const QString markerFolder = QDir(root).filePath("harmless-editor-bpm-marker");
    song(markerFolder, harmless);
    BeatmapDocument marker;
    require(marker.loadSong(markerFolder, &error) && marker.readOnlyReason().isEmpty(), "unchanged legacy editor BPM grid marker exemption retained");
    auto rootFlag = map3(); rootFlag.insert("bookmarksUseOfficialBpmEvents", true);
    const QString rootFlagFolder = QDir(root).filePath("unknown-root-bookmark-setting");
    song(rootFlagFolder, rootFlag);
    BeatmapDocument rootFlagDocument;
    require(rootFlagDocument.loadSong(rootFlagFolder, &error) && !rootFlagDocument.readOnlyReason().isEmpty(),
            "known bookmark exception is scoped to editor custom data");
    for (const int type : {10, 14, 15}) {
        const QString original = QDir(root).filePath("legacy-protected-event-" + QString::number(type));
        const QJsonObject map{{"_version", "2.5.0"}, {"_notes", QJsonArray{}}, {"_obstacles", QJsonArray{}},
                              {"_events", QJsonArray{QJsonObject{{"_time", 4}, {"_type", type}, {"_value", 120}}}}};
        song(original, map);
        BeatmapDocument document;
        require(document.loadSong(original, &error) && !document.readOnlyReason().isEmpty() &&
                !document.addObject(BeatObject{}, &error), "legacy type 10 and rotation events remain protected");
    }
    const QString shuffleFolder = QDir(root).filePath("legacy-shuffle-protection");
    song(shuffleFolder, map3());
    auto shuffleInfo = info(); shuffleInfo.insert("_shuffle", .25);
    require(ProjectStore::writeJson(QDir(shuffleFolder).filePath("Info.dat"), shuffleInfo, &error), error);
    BeatmapDocument shuffle;
    require(shuffle.loadSong(shuffleFolder, &error) && !shuffle.readOnlyReason().isEmpty() &&
            !shuffle.addObject(BeatObject{}, &error), "legacy shuffle remains protected");
}
void modernLightingAndColorTests(const QString &root) {
    QString error;
    const QJsonObject note2{{"_time", 1}, {"_lineIndex", 0}, {"_lineLayer", 0}, {"_type", 0}, {"_cutDirection", 1}};
    for (const QString &version : {QStringLiteral("2.5.0"), QStringLiteral("2.6.0")}) {
        auto ordinary = note2, endpoint = note2;
        endpoint.insert("_time", 2); endpoint.insert("_lineIndex", 1);
        const QJsonArray events{QJsonObject{{"_time", 0}, {"_type", 10}, {"_value", 1}, {"_floatValue", 1},
                                           {"_customData", QJsonObject{{"_color", QJsonArray{0.3, 0.7, 1.0, 1.0}}}}},
                                QJsonObject{{"_time", 2}, {"_type", 11}, {"_value", 7}, {"_floatValue", 0.8}},
                                QJsonObject{{"_time", 4}, {"_type", 100}, {"_value", 0}, {"_floatValue", 60}}};
        QJsonObject map{{"_version", version}, {"_notes", QJsonArray{ordinary, endpoint}}, {"_obstacles", QJsonArray{}},
                        {"_events", events}, {"unknown", QJsonObject{{"kept", true}}}};
        if (version == "2.6.0")
            map.insert("_sliders", QJsonArray{QJsonObject{{"_headTime", 2}, {"_headLineIndex", 1}, {"_headLineLayer", 0},
                                                        {"_tailTime", 3}, {"_tailLineIndex", 2}, {"_tailLineLayer", 0}}});
        const QString folder = QDir(root).filePath("modern-lighting-" + version);
        song(folder, map);
        BeatmapDocument document;
        require(document.loadSong(folder, &error), "modern type 10 load version=" + version + ": " + error);
        require(document.readOnlyReason().isEmpty(), "modern type 10 is lighting version=" + version +
                " reason=" + document.readOnlyReason());
        require(std::abs(document.timeMap().beatToSeconds(8) - 6.0) < 1e-10, "type 100 still applies its BPM; type 10 light does not retime objects");
        require(!document.objects()[0].isProtected(), "ordinary modern v2 note is editable");
        if (version == "2.6.0") {
            require(document.objects()[1].isProtected(), "modern lighting exemption keeps v2 arc endpoint protection");
            auto forged = document.objects()[1]; forged.protectedReason.clear();
            require(!document.updateObjects({document.objects()[0], forged}, &error) &&
                    !document.removeObjects({forged.id}, &error) &&
                    document.copyObjects({forged.id}, &error).isEmpty() &&
                    !document.pasteObjects({forged}, 8, 0, false, &error), "protected endpoint cannot bypass batch/copy/paste after modern lighting exemption");
        }
        auto edit = document.objects()[0]; edit.direction = 6;
        require(document.updateObject(edit, &error) && document.undo(), "modern lighting edit/undo");
        const QString undone = QDir(root).filePath("modern-lighting-undo-" + version);
        require(document.exportSong(undone, &error), error); compareFolders(folder, undone);
        require(document.redo(), "modern lighting redo");
        const QString saved = QDir(root).filePath("modern-lighting-project-" + version + "/project.lmsc");
        require(document.saveProject(saved, &error), error);
        BeatmapDocument reopened;
        require(reopened.loadProject(saved, &error) && reopened.readOnlyReason().isEmpty() &&
                reopened.objects()[0].direction == 6 && !reopened.objects()[0].isProtected(), "modern lighting project remains editable after save/reopen");
        const QString output = QDir(root).filePath("modern-lighting-export-" + version);
        require(reopened.exportSong(output, &error), error);
        auto expected = map; auto notes = expected.value("_notes").toArray();
        auto edited = notes[0].toObject(); edited.insert("_cutDirection", 6); notes[0] = edited; expected.insert("_notes", notes);
        QJsonObject actual;
        require(ProjectStore::readJson(QDir(output).filePath("Expert.dat"), &actual, &error) && actual == expected,
                "modern light/BPM/advanced/unknown fields are unchanged while one direction changes");
        require(hash(QDir(folder).filePath("song.ogg")) == hash(QDir(output).filePath("song.ogg")), "lighting fix keeps audio bytes");
    }
    for (const QString &version : {QStringLiteral("2.0.0"), QStringLiteral("2.2.0"), QStringLiteral("2.4.0"),
                                  QStringLiteral("2.5.0"), QStringLiteral("2.6.0-unknown"), QStringLiteral("2.6"),
                                  QStringLiteral("2.7.0")}) {
        const QString folder = QDir(root).filePath("ambiguous-type10-" + version);
        song(folder, {{"_version", version}, {"_notes", QJsonArray{note2}}, {"_obstacles", QJsonArray{}},
                      {"_events", QJsonArray{QJsonObject{{"_time", 2}, {"_type", 10}, {"_value", version == "2.5.0" ? 128 : 1}}}}});
        BeatmapDocument document;
        require(document.loadSong(folder, &error) && !document.readOnlyReason().isEmpty() &&
                !document.updateObject(document.objects()[0], &error), "old or non-light type 10 remains protected");
    }
    const QVector<QJsonValue> invalidLightValues{QStringLiteral("1"), 1.5, -1, 13};
    for (int i = 0; i < invalidLightValues.size(); ++i) {
        const QString folder = QDir(root).filePath("invalid-modern-type10-" + QString::number(i));
        song(folder, {{"_version", "2.6.0"}, {"_notes", QJsonArray{note2}}, {"_obstacles", QJsonArray{}},
                      {"_events", QJsonArray{QJsonObject{{"_time", 0}, {"_type", 10}, {"_value", invalidLightValues[i]}}}}});
        BeatmapDocument document;
        require(document.loadSong(folder, &error) && !document.readOnlyReason().isEmpty(), "malformed type 10 remains protected");
    }
    const QVector<QJsonValue> invalidLightCustom{QJsonArray{QJsonObject{{"_BPM", 180}}},
                                                QStringLiteral("unknown"), QJsonValue(QJsonValue::Null), true};
    for (int i = 0; i < invalidLightCustom.size(); ++i) {
        for (const QString &key : {QStringLiteral("_customData"), QStringLiteral("customData")}) {
            const QString folder = QDir(root).filePath("invalid-light-container-" + key + QString::number(i));
            song(folder, {{"_version", "2.6.0"}, {"_notes", QJsonArray{note2}}, {"_obstacles", QJsonArray{}},
                          {"_events", QJsonArray{QJsonObject{{"_time", 0}, {"_type", 10}, {"_value", 1}, {key, invalidLightCustom[i]}}}}});
            BeatmapDocument document;
            require(document.loadSong(folder, &error) && !document.readOnlyReason().isEmpty() &&
                    !document.updateObject(document.objects()[0], &error), "type 10 with malformed custom container remains protected");
        }
    }

    for (const bool v3 : {false, true}) {
        const QString prefix = v3 ? "static-color-v3" : "static-color-v2";
        const QString dataKey = v3 ? "customData" : "_customData", colorKey = v3 ? "color" : "_color";
        const QString noteKey = v3 ? "colorNotes" : "_notes";
        QJsonObject colored = v3 ? QJsonObject{{"b", 1}, {"x", 0}, {"y", 0}, {"c", 0}, {"d", 1}, {"a", 0}} : note2;
        const QJsonObject palette{{colorKey, QJsonArray{0.1, 0.6, 1.0, 0.5}}};
        colored.insert(dataKey, palette);
        auto animated = colored; animated.insert(v3 ? "b" : "_time", 2);
        auto animation = palette; animation.insert(v3 ? "track" : "_track", "linked-animation"); animated.insert(dataKey, animation);
        QJsonObject wall = v3 ? QJsonObject{{"b", 3}, {"x", 2}, {"y", 0}, {"d", 1}, {"w", 1}, {"h", 5}} :
                                   QJsonObject{{"_time", 3}, {"_lineIndex", 2}, {"_type", 0}, {"_duration", 1}, {"_width", 1}};
        wall.insert(dataKey, QJsonObject{{colorKey, QJsonArray{0.2, 0.3, 0.9}}});
        const QString wallKey = v3 ? "obstacles" : "_obstacles";
        QJsonObject map{{v3 ? "version" : "_version", v3 ? "3.3.0" : "2.6.0"},
                        {noteKey, QJsonArray{colored, animated}}, {wallKey, QJsonArray{wall}},
                        {v3 ? "bpmEvents" : "_events", QJsonArray{}}, {"unknown", QJsonArray{1, 2, 3}}};
        const QString folder = QDir(root).filePath(prefix); song(folder, map);
        require(ProjectStore::writeJson(QDir(folder).filePath("Hard.dat"), map3(), &error), error);
        auto metadata = info(); auto sets = metadata.value("_difficultyBeatmapSets").toArray();
        auto set = sets[0].toObject(); auto charts = set.value("_difficultyBeatmaps").toArray();
        charts.append(QJsonObject{{"_difficulty", "Hard"}, {"_difficultyRank", 5}, {"_beatmapFilename", "Hard.dat"}});
        set.insert("_difficultyBeatmaps", charts); sets[0] = set; metadata.insert("_difficultyBeatmapSets", sets);
        require(ProjectStore::writeJson(QDir(folder).filePath("Info.dat"), metadata, &error), error);
        BeatmapDocument document;
        require(document.loadSong(folder, &error) && document.readOnlyReason().isEmpty(), error);
        const auto baseline = document.objects();
        require(!baseline[0].isProtected() && baseline[1].isProtected() && !baseline[2].isProtected(), "only valid static RGB(A) is editable; animation remains protected");
        auto bad = baseline[0]; bad.preservedCustomData.insert(dataKey, animation);
        require(!document.updateObject(bad, &error) && !document.pasteObjects({bad}, 8, 0, false, &error), "opaque color carry cannot inject tracks through update or paste");
        bad = baseline[0]; bad.preservedCustomData = {}; bad.direction = 6;
        require(!document.updateObject(bad, &error), "basic editing cannot erase preserved colors");
        auto ordinary = baseline[0]; ordinary.direction = 6;
        auto advanced = baseline[1]; advanced.protectedReason.clear();
        require(!document.updateObjects({ordinary, advanced}, &error) &&
                !document.mirrorObjects({ordinary.id, advanced.id}, &error) &&
                !document.removeObjects({ordinary.id, advanced.id}, &error) &&
                document.copyObjects({advanced.id}, &error).isEmpty() &&
                !document.pasteObjects({advanced}, 8, 0, false, &error), "colored object mixed batches never bypass animation protection");
        require(document.updateObject(ordinary, &error) && document.undo(), error);
        const QString undone = QDir(root).filePath(prefix + "-undo");
        require(document.exportSong(undone, &error), error); compareFolders(folder, undone);
        require(document.redo() && document.mirrorObjects({ordinary.id}, &error) && document.undo(), "color data survives redo and mirror/undo");
        auto wallEdit = document.objects()[2]; wallEdit.duration = 1.25;
        require(document.updateObject(wallEdit, &error), "colored wall edit");
        const auto copied = document.copyObjects({ordinary.id}, &error);
        require(copied.size() == 1 && copied[0].preservedCustomData == ordinary.preservedCustomData &&
                document.pasteObjects(copied, 8, 0, false, &error), "copy/paste retains static color fields");
        require(document.removeObjects({wallEdit.id}, &error) && document.undo(), "colored wall delete/undo");
        const QString saved = QDir(root).filePath(prefix + "-project/project.lmsc");
        require(document.saveProject(saved, &error), error);
        BeatmapDocument reopened;
        require(reopened.loadProject(saved, &error) && reopened.readOnlyReason().isEmpty() &&
                reopened.objects().size() == 4 && !reopened.objects()[0].isProtected(), "static color edits and pasted colors survive project reopen");
        const QString output = QDir(root).filePath(prefix + "-export"); require(reopened.exportSong(output, &error), error);
        QJsonObject actual; require(ProjectStore::readJson(QDir(output).filePath("Expert.dat"), &actual, &error), error);
        auto expected = map; auto notes = expected.value(noteKey).toArray();
        auto edited = notes[0].toObject(); edited.insert(v3 ? "d" : "_cutDirection", 6); notes[0] = edited;
        auto pasted = edited; pasted.insert(v3 ? "b" : "_time", 9);
        if (!v3) pasted.remove("unknown");
        notes.append(pasted); expected.insert(noteKey, notes);
        auto walls = expected.value(wallKey).toArray(); auto editedWall = walls[0].toObject();
        editedWall.insert(v3 ? "d" : "_duration", 1.25); walls[0] = editedWall; expected.insert(wallKey, walls);
        require(actual == expected, "basic color edit/paste changes only intended basic fields and preserves original colors and animation JSON");
        for (const QString &file : {QStringLiteral("song.ogg"), QStringLiteral("Hard.dat"), QStringLiteral("Info.dat")})
            require(hash(QDir(folder).filePath(file)) == hash(QDir(output).filePath(file)), "colors preserve audio, other difficulty and metadata bytes");
        require(reopened.setDifficulty(reopened.difficulties()[1].id, &error), error);
        const auto crossCopy = copied;
        require(reopened.pasteObjects(crossCopy, 16, 0, false, &error), "static palette copies between v2/v3 tracks");
        const QString crossOutput = QDir(root).filePath(prefix + "-cross-export"); require(reopened.exportSong(crossOutput, &error), error);
        QJsonObject cross; require(ProjectStore::readJson(QDir(crossOutput).filePath("Hard.dat"), &cross, &error), error);
        const auto crossNote = cross.value("colorNotes").toArray().last().toObject();
        require(crossNote.value("customData").toObject().value("color") == palette.value(colorKey) &&
                !crossNote.contains("_customData"), "cross-schema palette uses target schema without losing color");

        QJsonObject manifest; require(ProjectStore::readJson(saved, &manifest, &error), error);
        const auto unmodifiedManifest = manifest;
        auto difficulties = manifest.value("difficulties").toArray(); auto chart = difficulties[0].toObject();
        auto edits = chart.value("edits").toArray();
        bool forgedAddition = false;
        for (int i = 0; i < edits.size(); ++i) {
            auto edit = edits[i].toObject(); if (edit.value("index").toInt() != -1) continue;
            edit.insert("preservedCustomData", QJsonObject{{dataKey, animation}}); edits[i] = edit; forgedAddition = true; break;
        }
        require(forgedAddition, "forged project has pasted object fixture");
        chart.insert("edits", edits); difficulties[0] = chart; manifest.insert("difficulties", difficulties);
        require(ProjectStore::writeJson(saved, manifest, &error), error);
        BeatmapDocument forgedProject;
        require(!forgedProject.loadProject(saved, &error), "project load cannot inject animation through preserved colors");
        manifest = unmodifiedManifest; difficulties = manifest.value("difficulties").toArray();
        chart = difficulties[0].toObject(); edits = chart.value("edits").toArray();
        bool changedOriginalColor = false;
        for (int i = 0; i < edits.size(); ++i) {
            auto edit = edits[i].toObject(); if (edit.value("index").toInt() != 0 || edit.value("array").toString() != noteKey) continue;
            edit.insert("preservedCustomData", QJsonObject{{dataKey, QJsonObject{{colorKey, QJsonArray{1.0, 0.0, 0.0}}}}});
            edits[i] = edit; changedOriginalColor = true; break;
        }
        require(changedOriginalColor, "forged existing-color edit fixture");
        chart.insert("edits", edits); difficulties[0] = chart; manifest.insert("difficulties", difficulties);
        require(ProjectStore::writeJson(saved, manifest, &error) && !forgedProject.loadProject(saved, &error),
                "project restore cannot replace original static colors through forged edit records");
    }
    const QVector<QJsonValue> invalidColors{QStringLiteral("pointReference"), QJsonArray{1, 0}, QJsonArray{1, 0, 0, 1, 0},
                                          QJsonArray{1, QStringLiteral("0"), 0}, QJsonArray{1, 0, QJsonValue(QJsonValue::Null)},
                                          QJsonArray{1, 0, -1}, QJsonArray{1, 0, 2}};
    for (int i = 0; i < invalidColors.size(); ++i) {
        auto note = note2; note.insert("_customData", QJsonObject{{"_color", invalidColors[i]}});
        const QString folder = QDir(root).filePath("invalid-static-color-" + QString::number(i));
        song(folder, {{"_version", "2.6.0"}, {"_notes", QJsonArray{note}}, {"_obstacles", QJsonArray{}}, {"_events", QJsonArray{}}});
        BeatmapDocument document;
        require(document.loadSong(folder, &error) && document.objects()[0].isProtected() &&
                !document.updateObject(document.objects()[0], &error), "malformed, animated or out-of-range color remains protected");
    }
    const QVector<QJsonValue> unsupportedCustom{
        QJsonValue(QJsonValue::Null), QJsonArray{}, QStringLiteral("unknown"), true,
        QJsonObject{{"_color", QJsonArray{1, 0, 0}}, {"_animation", QJsonObject{}}},
        QJsonObject{{"_color", QJsonArray{1, 0, 0}}, {"_position", QJsonArray{0, 0}}},
        QJsonObject{{"_color", QJsonArray{1, 0, 0}}, {"_interactable", false}}};
    for (int i = 0; i < unsupportedCustom.size(); ++i) {
        auto note = note2; note.insert("_customData", unsupportedCustom[i]);
        const QString folder = QDir(root).filePath("unsupported-object-custom-" + QString::number(i));
        song(folder, {{"_version", "2.6.0"}, {"_notes", QJsonArray{note}}, {"_obstacles", QJsonArray{}}, {"_events", QJsonArray{}}});
        BeatmapDocument document;
        require(document.loadSong(folder, &error) && document.objects()[0].isProtected(), "malformed custom container and color with advanced fields remain protected");
        const QString saved = QDir(root).filePath("unsupported-object-project-" + QString::number(i));
        require(document.saveProject(saved, &error), error);
        BeatmapDocument reopened;
        require(reopened.loadProject(saved, &error) && reopened.objects()[0].isProtected(), "save/reopen retains custom data protection");
        const QString output = QDir(root).filePath("unsupported-object-export-" + QString::number(i));
        require(reopened.exportSong(output, &error), error); compareFolders(folder, output);
    }
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
    editorTimingTests(root);
    modernLightingAndColorTests(root);

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
// Optional local regression: copies the entire project first and never saves or
// exports back into the source. No user fixture or identifying path is tracked.
void existingProjectRegression(const QString &manifestPath, const QString &root) {
    QString error;
    const QString original = QFileInfo(manifestPath).absolutePath();
    const auto sourceFiles = ProjectStore::files(original, &error);
    require(error.isEmpty() && !sourceFiles.isEmpty(), "local source project files: " + error);
    QHash<QString, QByteArray> before;
    for (const auto &relative : sourceFiles) before.insert(relative, hash(QDir(original).filePath(relative)));
    QJsonObject manifest;
    require(ProjectStore::readJson(manifestPath, &manifest, &error), error);
    const QString originalAssets = QDir(original).filePath(manifest.value("assets").toString());
    const QString copy = QDir(root).filePath("existing-project-copy");
    require(ProjectStore::copyTree(original, copy, &error), error);
    const QString copiedManifest = QDir(copy).filePath(QFileInfo(manifestPath).fileName());
    BeatmapDocument initial;
    require(initial.loadProject(copiedManifest, &error), error);
    const auto difficulties = initial.difficulties();
    for (int index = 0; index < difficulties.size(); ++index) {
        const auto &difficulty = difficulties[index];
        BeatmapDocument document;
        require(document.loadProject(copiedManifest, &error) && document.setDifficulty(difficulty.id, &error), error);
        require(document.readOnlyReason().isEmpty(), "local difficulty remains locked: " + document.readOnlyReason());
        require(difficulty.version.startsWith("2.") || difficulty.version.startsWith("3."), "local regression requires v2/v3 chart");
        const bool v3 = difficulty.version.startsWith("3.");
        BeatObject edit;
        bool found = false;
        for (const auto &object : document.objects()) if (object.kind == ObjectKind::Note && !object.isProtected()) {
            edit = object; found = true; break;
        }
        require(found, "local chart has editable basic note");
        edit.direction = (edit.direction + 1) % 9;
        require(document.updateObject(edit, &error) && document.undo(), "local basic edit and undo: " + error);
        const QString undone = QDir(root).filePath("existing-undo-" + QString::number(index));
        require(document.exportSong(undone, &error), error);
        compareFolders(originalAssets, undone);
        require(document.redo(), "local basic edit redo");
        BeatObject coloredEdit;
        bool coloredFound = false;
        for (const auto &object : document.objects())
            if (!object.isProtected() && !object.preservedCustomData.isEmpty() && object.id != edit.id) {
                coloredEdit = object; coloredFound = true; break;
            }
        if (coloredFound) {
            if (coloredEdit.kind == ObjectKind::Wall) coloredEdit.duration += 0.125;
            else if (coloredEdit.kind == ObjectKind::Note) coloredEdit.direction = (coloredEdit.direction + 1) % 9;
            else coloredEdit.beat += 0.125;
            require(document.updateObject(coloredEdit, &error), "local static-color object is editable: " + error);
        }
        const QString savedPath = QDir(root).filePath("existing-saved-" + QString::number(index) + "/project.lmsc");
        require(document.saveProject(savedPath, &error), error);
        BeatmapDocument reopened;
        require(reopened.loadProject(savedPath, &error) && reopened.readOnlyReason().isEmpty(), "local saved project is editable: " + error);
        bool restored = false;
        for (const auto &object : reopened.objects())
            if (object.id == edit.id && object.direction == edit.direction) restored = true;
        require(restored, "local project saved basic edit survives reopen");
        if (coloredFound) {
            bool colorRestored = false;
            for (const auto &object : reopened.objects())
                if (object.id == coloredEdit.id && !object.isProtected() &&
                    object.preservedCustomData == coloredEdit.preservedCustomData &&
                    object.duration == coloredEdit.duration && object.direction == coloredEdit.direction &&
                    object.beat == coloredEdit.beat) colorRestored = true;
            require(colorRestored, "local project retains editable color object and original color after reopen");
        }
        const QString output = QDir(root).filePath("existing-edited-" + QString::number(index));
        require(reopened.exportSong(output, &error), error);
        for (const auto &relative : ProjectStore::files(originalAssets)) if (relative != difficulty.filename)
            require(hash(QDir(originalAssets).filePath(relative)) == hash(QDir(output).filePath(relative)),
                    "local edit changed unrelated map/audio/cover/metadata: " + relative);
        QJsonObject oldMap, newMap;
        require(ProjectStore::readJson(QDir(originalAssets).filePath(difficulty.filename), &oldMap, &error) &&
                ProjectStore::readJson(QDir(output).filePath(difficulty.filename), &newMap, &error), error);
        auto expected = oldMap;
        const auto replaceBasicField = [&expected](const BeatObject &object, const QString &field, double value) {
            const auto parts = object.id.split(':');
            require(parts.size() == 3, "local color-object identifier");
            const QString array = parts[1]; const int row = parts[2].toInt();
            auto rows = expected.value(array).toArray(); auto rawObject = rows[row].toObject();
            rawObject.insert(field, value); rows[row] = rawObject; expected.insert(array, rows);
        };
        replaceBasicField(edit, v3 ? "d" : "_cutDirection", edit.direction);
        if (coloredFound)
            replaceBasicField(coloredEdit, coloredEdit.kind == ObjectKind::Wall ? (v3 ? "d" : "_duration") :
                              coloredEdit.kind == ObjectKind::Note ? (v3 ? "d" : "_cutDirection") : (v3 ? "b" : "_time"),
                              coloredEdit.kind == ObjectKind::Wall ? coloredEdit.duration :
                              coloredEdit.kind == ObjectKind::Note ? coloredEdit.direction : coloredEdit.beat);
        require(expected == newMap, "local edit changes only chosen basic fields; colors, timing, unknown JSON and advanced arrays stay identical");
        QTextStream(stdout) << "PASS local chart " << difficulty.filename
                            << ": editable, undo byte-identical, redo/save/reopen/export, only chosen basic fields changed; static-color edit "
                            << (coloredFound ? "verified" : "not present") << "\n";
    }
    require(ProjectStore::files(original) == sourceFiles, "local source project file set unchanged");
    for (const auto &relative : sourceFiles) {
        const auto after = hash(QDir(original).filePath(relative));
        require(after == before.value(relative), "local source project was modified: " + relative);
        QTextStream(stdout) << "PASS source SHA256 unchanged: " << relative << " " << after.toHex() << "\n";
    }
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary(QDir::tempPath() + "/lmsc-core-test-XXXXXX");
    try {
        require(temporary.isValid(), "test temporary directory");
        unitTests(temporary.path());
        QTextStream(stdout) << "PASS synthetic edit, preservation, protection, undo/redo, project/recovery, BPM tests\n";
        const auto arguments = application.arguments();
        if (arguments.size() > 2 && arguments[1] == "--project-regression")
            existingProjectRegression(arguments[2], temporary.path());
        else if (arguments.size() > 1) realSamples(arguments[1], temporary.path());
        return 0;
    } catch (const std::exception &exception) {
        QTextStream(stderr) << "FAIL " << QString::fromUtf8(exception.what()) << "\n";
        return 1;
    }
}
