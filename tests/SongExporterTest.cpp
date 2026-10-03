#include "core/BeatmapDocument.h"
#include "core/ProjectStore.h"
#include "core/SongExporter.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace lmsc;

namespace {
constexpr int SampleRate = 44100;
constexpr int Channels = 2;
void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toUtf8().constData());
}
QJsonObject readJson(const QString &path) {
    QJsonObject object;
    QString error;
    require(ProjectStore::readJson(path, &object, &error), error);
    return object;
}
QByteArray hash(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "open hash input: " + path);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    require(digest.addData(&file), "hash input: " + path);
    return digest.result();
}
QHash<QString, QByteArray> folderHashes(const QString &folder) {
    QHash<QString, QByteArray> hashes;
    for (const auto &relative : ProjectStore::files(folder)) hashes.insert(relative, hash(QDir(folder).filePath(relative)));
    return hashes;
}
void runFfmpeg(const QString &program, const QStringList &arguments) {
    QProcess process;
    process.start(program, arguments);
    require(process.waitForStarted(10000), "start fixture FFmpeg: " + process.errorString());
    if (!process.waitForFinished(20000)) {
        process.kill(); process.waitForFinished();
        require(false, "fixture FFmpeg timeout");
    }
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            "fixture FFmpeg: " + QString::fromUtf8(process.readAllStandardError()));
}
void writeSyntheticWave(const QString &path, int sampleRate = SampleRate) {
    // Distinct signals in both channels detect a delay applied to only one
    // channel. This fixture contains no user media and lives in a temp folder.
    const int frames = sampleRate * 4;
    const quint32 pcmBytes = frames * Channels * sizeof(qint16);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create synthetic wave");
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4); stream << quint32(36 + pcmBytes);
    stream.writeRawData("WAVEfmt ", 8); stream << quint32(16) << quint16(1) << quint16(Channels);
    stream << quint32(sampleRate) << quint32(sampleRate * Channels * sizeof(qint16));
    stream << quint16(Channels * sizeof(qint16)) << quint16(16);
    stream.writeRawData("data", 4); stream << pcmBytes;
    constexpr double pi = 3.14159265358979323846;
    for (int frame = 0; frame < frames; ++frame) {
        const double t = double(frame) / sampleRate;
        const double envelope = 0.75 + 0.2 * std::sin(2 * pi * 2.3 * t);
        stream << qint16(10000 * envelope * std::sin(2 * pi * 440 * t));
        stream << qint16(8500 * envelope * std::sin(2 * pi * 661 * t + 0.7));
    }
    require(stream.status() == QDataStream::Ok, "write synthetic PCM");
}
QVector<qint16> decode(const QString &ffmpeg, const QString &source, const QString &output) {
    runFfmpeg(ffmpeg, {"-v", "error", "-nostdin", "-y", "-i", source, "-map", "0:a:0", "-ac", "2", "-ar", "44100",
                       "-f", "s16le", output});
    QFile file(output);
    require(file.open(QIODevice::ReadOnly), "open decoded PCM");
    const auto bytes = file.readAll();
    require(!bytes.isEmpty() && bytes.size() % (Channels * int(sizeof(qint16))) == 0, "decoded PCM frame size");
    QVector<qint16> samples;
    samples.reserve(bytes.size() / int(sizeof(qint16)));
    for (int offset = 0; offset < bytes.size(); offset += sizeof(qint16))
        samples.append(qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(bytes.constData() + offset)));
    return samples;
}
void verifyAudioDelay(const QVector<qint16> &original, const QVector<qint16> &delayed, double seconds) {
    const int frames = qRound(seconds * SampleRate);
    const int originalFrames = original.size() / Channels;
    const int delayedFrames = delayed.size() / Channels;
    require(std::abs(delayedFrames - originalFrames - frames) <= 2048, "padded decoded duration equals original plus lead-in");
    require(delayedFrames > frames + SampleRate, "padded audio contains the delayed song");
    for (int channel = 0; channel < Channels; ++channel) {
        double silentEnergy = 0;
        for (int frame = 0; frame < frames; ++frame) {
            const double sample = delayed[frame * Channels + channel];
            silentEnergy += sample * sample;
        }
        // Vorbis can ring briefly around the boundary; use RMS over the full
        // lead-in instead of demanding byte-perfect zeroes after recompression.
        require(std::sqrt(silentEnergy / frames) < 90, "both channels are nearly silent throughout the lead-in");
        double cross = 0, originalEnergy = 0, delayedEnergy = 0;
        for (int frame = SampleRate / 10; frame < SampleRate * 3; ++frame) {
            const double a = original[frame * Channels + channel];
            const double b = delayed[(frames + frame) * Channels + channel];
            cross += a * b; originalEnergy += a * a; delayedEnergy += b * b;
        }
        require(originalEnergy > 0 && delayedEnergy > 0 && cross / std::sqrt(originalEnergy * delayedEnergy) > .98,
                "each delayed channel matches the original signal at the exact sample offset");
    }
}
void verifyShift(const QJsonObject &before, const QJsonObject &after, double beatShift) {
    for (const QString &key : {QStringLiteral("_notes"), QStringLiteral("_obstacles"), QStringLiteral("_events"), QStringLiteral("_waypoints")}) {
        const auto original = before.value(key).toArray(), shifted = after.value(key).toArray();
        require(original.size() == shifted.size(), "export retains all objects: " + key);
        for (int i = 0; i < original.size(); ++i) {
            auto expected = original[i].toObject();
            expected.insert("_time", expected.value("_time").toDouble() + beatShift);
            const auto actual = shifted[i].toObject();
            require(std::abs(actual.value("_time").toDouble() - expected.value("_time").toDouble()) < 1e-9,
                    "export shifts every object's start time: " + key);
            expected.remove("_time"); auto remaining = actual; remaining.remove("_time");
            require(expected == remaining, "export preserves object properties and durations: " + key);
        }
    }
}

void verifyMultipleDifficulties(const QString &tools, const QString &root, const QString &sourceProject,
                                double leadIn, double bpm, const QVector<qint16> &originalPcm) {
    QString error;
    BeatmapDocument document;
    require(document.loadProject(sourceProject, &error), error);
    const QStringList names{"Hard", "Normal", "Expert", "ExpertPlus"};
    const QVector<int> ranks{5, 3, 7, 9};
    for (int i = 0; i < names.size(); ++i) {
        BeatObject note; note.beat = 1 + i; note.x = i % 4; note.y = 1; note.color = i % 2; note.direction = 1;
        BeatObject wall; wall.kind = ObjectKind::Wall; wall.beat = 5 + i; wall.x = 0; wall.y = 0; wall.width = 1; wall.duration = .5;
        require(document.applyGeneratedChart({note, wall}, names[i], ranks[i], document.revision(), &error), error);
    }
    require(document.difficulties().size() == 5, "generating four more difficulties retains the original Easy chart");
    const QString project = QDir(root).filePath("multi-project");
    require(document.saveProject(project, &error), error);
    const auto savedHashes = folderHashes(project);
    const QString beforeSelection = document.currentDifficultyId();
    const QString baseline = QDir(root).filePath("multi-baseline"), padded = QDir(root).filePath("multi-padded");
    require(SongExporter::exportSong(document, baseline, 0, "missing-tools", &error), error);
    require(SongExporter::exportSong(document, padded, leadIn, tools, &error), error);
    const auto beforeInfo = readJson(QDir(baseline).filePath("Info.dat"));
    const auto afterInfo = readJson(QDir(padded).filePath("Info.dat"));
    const auto beforeDifficulties = beforeInfo.value("_difficultyBeatmapSets").toArray().first().toObject().value("_difficultyBeatmaps").toArray();
    require(beforeDifficulties.size() == 5 && beforeInfo.value("_difficultyBeatmapSets") == afterInfo.value("_difficultyBeatmapSets"),
            "lead-in preserves all five difficulty descriptors and filenames");
    for (const auto &value : beforeDifficulties) {
        const auto difficulty = value.toObject();
        const QString name = difficulty.value("_difficulty").toString();
        const QString filename = difficulty.value("_beatmapFilename").toString();
        require(filename == name + ".dat", "each exported difficulty has its own standard filename");
        verifyShift(readJson(QDir(baseline).filePath(filename)), readJson(QDir(padded).filePath(filename)), leadIn * bpm / 60);
    }
    verifyAudioDelay(originalPcm, decode(QDir(tools).filePath("ffmpeg.exe"), QDir(padded).filePath("song.ogg"),
                                       QDir(root).filePath("multi-padded.pcm")), leadIn);
    require(document.currentDifficultyId() == beforeSelection && !document.isModified() && folderHashes(project) == savedHashes,
            "multi-difficulty export leaves selection, saved assets, and dirty state unchanged");
    BeatmapDocument exported;
    require(exported.loadSong(padded, &error) && exported.difficulties().size() == 5, error);
    const auto difficulties = document.difficulties();
    for (const auto &difficulty : difficulties) {
        require(document.setDifficulty(difficulty.id, &error), error);
        QString exportedId;
        for (const auto &candidate : exported.difficulties()) if (candidate.name == difficulty.name) exportedId = candidate.id;
        require(!exportedId.isEmpty() && exported.setDifficulty(exportedId, &error), error);
        require(document.objects().size() == exported.objects().size(), "all padded charts reload without missing objects");
        for (int i = 0; i < document.objects().size(); ++i)
            require(std::abs(exported.timeMap().beatToSeconds(exported.objects()[i].beat)
                             - document.timeMap().beatToSeconds(document.objects()[i].beat) - leadIn) < 1e-9,
                    "every padded difficulty has the same gameplay-time shift");
    }
    BeatmapDocument reopened;
    require(reopened.loadProject(project, &error), error);
    const QString repeated = QDir(root).filePath("multi-repeated");
    require(SongExporter::exportSong(reopened, repeated, leadIn, tools, &error), error);
    for (const auto &value : beforeDifficulties) {
        const QString filename = value.toObject().value("_beatmapFilename").toString();
        require(readJson(QDir(repeated).filePath(filename)) == readJson(QDir(padded).filePath(filename)),
                "multi-difficulty save/reopen exports do not accumulate lead-in");
    }

    // Inject protected content into the non-selected original Easy map. The
    // saved source snapshot deliberately remains the original Expert.dat.
    for (int variant = 0; variant < 7; ++variant) {
        const QString variantProject = QDir(root).filePath(QString("multi-protected-project-%1").arg(variant));
        require(ProjectStore::copyTree(project, variantProject, &error), error);
        const QString manifestPath = QDir(variantProject).filePath("project.lmsc");
        auto manifest = readJson(manifestPath);
        const QString mapPath = QDir(variantProject).filePath(manifest.value("assets").toString() + "/Expert.dat");
        auto map = readJson(mapPath);
        if (variant == 0)
            map.insert("_events", QJsonArray{QJsonObject{{"_time", 2}, {"_type", 100}, {"_floatValue", 180}}});
        else if (variant == 1)
            map.insert("_events", QJsonArray{QJsonObject{{"_time", 2}, {"_type", 14}, {"_value", 1}}});
        else if (variant == 2)
            map.insert("_customData", QJsonObject{{"_customEvents", QJsonArray{QJsonObject{{"_time", 1}, {"_type", "AnimateTrack"}}}}});
        else if (variant == 3)
            map.insert("_notes", QJsonArray{QJsonObject{{"_time", 1}, {"_lineIndex", 3}, {"_lineLayer", 2}, {"_type", 0},
                                                      {"_cutDirection", 8}, {"customData", QJsonObject{{"animation", "keep"}}}}});
        else if (variant == 4)
            map.insert("_obstacles", QJsonArray{QJsonObject{{"_time", 1}, {"_lineIndex", 0}, {"_type", 0}, {"_duration", 1}, {"_width", 5}}});
        else if (variant == 5)
            map.insert("customData", "malformed mod data must not be silently discarded");
        else {
            auto info = manifest.value("info").toObject();
            auto sets = info.value("_difficultyBeatmapSets").toArray();
            auto set = sets.first().toObject();
            auto descriptors = set.value("_difficultyBeatmaps").toArray();
            for (int i = 0; i < descriptors.size(); ++i) {
                auto descriptor = descriptors[i].toObject();
                if (descriptor.value("_difficulty").toString() == "Easy") {
                    descriptor.insert("customData", QJsonObject{{"keep", true}}); descriptors[i] = descriptor;
                }
            }
            set.insert("_difficultyBeatmaps", descriptors); sets[0] = set; info.insert("_difficultyBeatmapSets", sets);
            manifest.insert("info", info);
        }
        require(ProjectStore::writeJson(mapPath, map, &error), error);
        auto assetHashes = manifest.value("assetHashes").toObject();
        assetHashes.insert("Expert.dat", QString::fromLatin1(hash(mapPath).toHex())); manifest.insert("assetHashes", assetHashes);
        auto stateDifficulties = manifest.value("difficulties").toArray();
        for (int i = 0; i < stateDifficulties.size(); ++i) {
            auto difficulty = stateDifficulties[i].toObject();
            if (difficulty.value("file").toString() == "Expert.dat") {
                difficulty.insert("edits", QJsonArray{}); stateDifficulties[i] = difficulty;
            }
        }
        manifest.insert("difficulties", stateDifficulties);
        require(ProjectStore::writeJson(manifestPath, manifest, &error), error);
        const auto originalHashes = folderHashes(variantProject);
        BeatmapDocument protectedSong;
        require(protectedSong.loadProject(variantProject, &error), error);
        QString hardId;
        for (const auto &difficulty : protectedSong.difficulties()) if (difficulty.name == "Hard") hardId = difficulty.id;
        require(!hardId.isEmpty() && protectedSong.setDifficulty(hardId, &error), error);
        require(protectedSong.readOnlyReason().isEmpty() && protectedSong.timeMap().changes().isEmpty(),
                "selected Hard chart is editable while Easy contains protected content");
        const QString output = QDir(root).filePath(QString("multi-protected-output-%1").arg(variant));
        require(!SongExporter::exportSong(protectedSong, output, leadIn, tools, &error) && !QFileInfo::exists(output),
                "protected content in a non-selected difficulty blocks the entire lead-in export");
        const QString zeroOutput = QDir(root).filePath(QString("multi-protected-zero-output-%1").arg(variant));
        require(SongExporter::exportSong(protectedSong, zeroOutput, 0, "missing-tools", &error) &&
                readJson(QDir(zeroOutput).filePath("Easy.dat")) == map,
                "zero lead-in preserves protected content in non-selected difficulties");
        require(folderHashes(variantProject) == originalHashes && protectedSong.currentDifficultyId() == hardId,
                "rejected multi-chart padding does not alter source snapshots or current selection");
    }
}

void runTests(const QString &tools, const QString &root) {
    const QString ffmpeg = QDir(tools).filePath("ffmpeg.exe");
    const QString wav = QDir(root).filePath("synthetic-stereo.wav"), ogg = QDir(root).filePath("synthetic-stereo.ogg");
    writeSyntheticWave(wav);
    runFfmpeg(ffmpeg, {"-v", "error", "-nostdin", "-y", "-i", wav, "-c:a", "libvorbis", "-q:a", "5", ogg});
    const auto originalPcm = decode(ffmpeg, ogg, QDir(root).filePath("original.pcm"));
    QString error;
    BeatmapDocument document;
    constexpr double bpm = 120.627, firstBeat = .035, leadIn = 2;
    require(document.createNew(ogg, "合成导出测试", bpm, firstBeat, {}, &error) &&
            document.setNewSongDifficulty("Easy", 1, &error), error);
    for (int i = 0; i < 10; ++i) {
        BeatObject note; note.beat = .25 + i * .5; note.x = 1; note.y = 1; note.color = 0;
        require(document.addObject(note, &error), error);
    }
    for (int i = 0; i < 5; ++i) {
        BeatObject note; note.beat = .75 + i; note.x = 2; note.y = 1; note.color = 1;
        require(document.addObject(note, &error), error);
    }
    int earlyNotes = 0;
    for (const auto &note : document.objects()) if (document.timeMap().beatToSeconds(note.beat) < 1) ++earlyNotes;
    require(document.objects().size() == 15 && earlyNotes == 6, "reproduce the screenshot's 15 notes, including six in the first second");
    BeatObject bomb; bomb.kind = ObjectKind::Bomb; bomb.beat = .5; bomb.x = 0; bomb.y = 2;
    BeatObject wall; wall.kind = ObjectKind::Wall; wall.beat = 1; wall.x = 0; wall.y = 0; wall.width = 1; wall.duration = .5;
    require(document.addObject(bomb, &error) && document.addObject(wall, &error), error);
    const QString project = QDir(root).filePath("project");
    require(document.saveProject(project, &error), error);
    const QString manifestPath = QDir(project).filePath("project.lmsc");
    auto manifest = readJson(manifestPath);
    const QString assets = QDir(project).filePath(manifest.value("assets").toString());
    // Exercise ordinary events/waypoints and nonzero preview metadata without
    // adding public editing APIs solely for this test.
    const QString mapPath = QDir(assets).filePath("Expert.dat");
    auto map = readJson(mapPath);
    map.insert("_events", QJsonArray{QJsonObject{{"_time", .5}, {"_type", 0}, {"_value", 1}}});
    map.insert("_waypoints", QJsonArray{QJsonObject{{"_time", 1.5}, {"_lineIndex", 0}, {"_lineLayer", 0}, {"_offsetDirection", 1}}});
    require(ProjectStore::writeJson(mapPath, map, &error), error);
    auto assetHashes = manifest.value("assetHashes").toObject();
    assetHashes.insert("Expert.dat", QString::fromLatin1(hash(mapPath).toHex())); manifest.insert("assetHashes", assetHashes);
    auto metadata = manifest.value("info").toObject();
    metadata.insert("_previewStartTime", .75); metadata.insert("_songApproximativeDuration", 4);
    manifest.insert("info", metadata);
    require(ProjectStore::writeJson(manifestPath, manifest, &error), error);
    BeatmapDocument reopened;
    require(reopened.loadProject(project, &error), error);
    const auto sourceHashes = folderHashes(project);
    const auto originalAudioHash = hash(reopened.audioPath());
    const QString baseline = QDir(root).filePath("baseline"), padded = QDir(root).filePath("padded");
    require(SongExporter::exportSong(reopened, baseline, 0, "missing-tools", &error), error);
    require(SongExporter::exportSong(reopened, padded, leadIn, tools, &error), error);
    const auto beforeMap = readJson(QDir(baseline).filePath("Easy.dat")), afterMap = readJson(QDir(padded).filePath("Easy.dat"));
    require(afterMap.value("_notes").toArray().size() == 16 && afterMap.value("_obstacles").toArray().size() == 1,
            "padding retains 15 color notes, a bomb, and a wall");
    verifyShift(beforeMap, afterMap, leadIn * bpm / 60);
    const auto beforeInfo = readJson(QDir(baseline).filePath("Info.dat")), afterInfo = readJson(QDir(padded).filePath("Info.dat"));
    require(std::abs(afterInfo.value("_previewStartTime").toDouble() - beforeInfo.value("_previewStartTime").toDouble() - leadIn) < 1e-9 &&
            std::abs(afterInfo.value("_songApproximativeDuration").toDouble() - 6) < 1e-9 &&
            afterInfo.value("_songTimeOffset").toDouble() == 0, "preview and positive duration follow padded audio, with zero deprecated offset");
    const auto delayedPcm = decode(ffmpeg, QDir(padded).filePath("song.ogg"), QDir(root).filePath("padded.pcm"));
    verifyAudioDelay(originalPcm, delayedPcm, leadIn);
    BeatmapDocument exported;
    require(exported.loadSong(padded, &error), error);
    const auto &beforeObjects = reopened.objects(), &afterObjects = exported.objects();
    require(beforeObjects.size() == afterObjects.size(), "padded map reload object count");
    for (int i = 0; i < beforeObjects.size(); ++i)
        require(std::abs(exported.timeMap().beatToSeconds(afterObjects[i].beat) - reopened.timeMap().beatToSeconds(beforeObjects[i].beat) - leadIn) < 1e-9,
                "padded object gameplay time equals editor time plus lead-in");
    require(folderHashes(project) == sourceHashes && hash(reopened.audioPath()) == originalAudioHash && !reopened.isModified(),
            "padding never mutates saved project, assets, source audio, or dirty state");
    BeatmapDocument again;
    require(again.loadProject(project, &error), error);
    const QString repeated = QDir(root).filePath("repeated");
    require(SongExporter::exportSong(again, repeated, leadIn, tools, &error), error);
    require(readJson(QDir(repeated).filePath("Easy.dat")) == afterMap && readJson(QDir(repeated).filePath("Info.dat")) == afterInfo,
            "save/reopen and repeated padding never accumulate the lead-in");
    const QString disabled = QDir(root).filePath("disabled");
    require(SongExporter::exportSong(again, disabled, 0, "missing-tools", &error) && folderHashes(disabled) == folderHashes(baseline),
            "zero lead-in exports the original new-song bytes without needing FFmpeg");
    const QString missing = QDir(root).filePath("missing-tools-output");
    require(!SongExporter::exportSong(again, missing, leadIn, QDir(root).filePath("missing-tools"), &error) && !QFileInfo::exists(missing),
            "missing FFmpeg fails without committing a target folder");
    const QString corruptAudio = QDir(root).filePath("corrupt.ogg");
    QFile corrupt(corruptAudio);
    require(corrupt.open(QIODevice::WriteOnly) && corrupt.write("not an Ogg stream") > 0, "create corrupted audio fixture");
    corrupt.close();
    BeatmapDocument corruptSong;
    require(corruptSong.createNew(corruptAudio, "坏音频测试", 120, 0, {}, &error), error);
    const QString corruptOutput = QDir(root).filePath("corrupt-audio-output");
    require(!SongExporter::exportSong(corruptSong, corruptOutput, leadIn, tools, &error) && !QFileInfo::exists(corruptOutput),
            "an actual FFmpeg conversion failure never commits the target folder");
    int invalidIndex = 0;
    for (double invalid : {-1., 10.001, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        const QString output = QDir(root).filePath(QString("invalid-%1").arg(invalidIndex++));
        require(!SongExporter::exportSong(again, output, invalid, tools, &error) && !QFileInfo::exists(output), "invalid lead-in values are rejected atomically");
    }
    const auto existingHashes = folderHashes(padded);
    require(!SongExporter::exportSong(again, padded, leadIn, tools, &error) && folderHashes(padded) == existingHashes,
            "an existing target is never overwritten");
    BeatmapDocument imported;
    require(imported.loadSong(baseline, &error), error);
    const QString importedOut = QDir(root).filePath("imported");
    require(SongExporter::exportSong(imported, importedOut, leadIn, "missing-tools", &error) && folderHashes(importedOut) == folderHashes(baseline),
            "imported game songs are byte-identical even when a lead-in is requested");
    // A source rate other than the export rate catches adelay sample counts
    // interpreted before resampling. Unknown duration must remain unknown.
    const QString highRateWav = QDir(root).filePath("synthetic-48000.wav"), highRateOgg = QDir(root).filePath("synthetic-48000.ogg");
    writeSyntheticWave(highRateWav, 48000);
    runFfmpeg(ffmpeg, {"-v", "error", "-nostdin", "-y", "-i", highRateWav, "-c:a", "libvorbis", "-q:a", "5", highRateOgg});
    BeatmapDocument highRate;
    require(highRate.createNew(highRateOgg, "48k 合成测试", 120, 0, {}, &error), error);
    const QString highRateOutput = QDir(root).filePath("high-rate-output");
    require(SongExporter::exportSong(highRate, highRateOutput, leadIn, tools, &error), error);
    const auto highRateOriginal = decode(ffmpeg, highRateOgg, QDir(root).filePath("high-rate-original.pcm"));
    const auto highRateDelayed = decode(ffmpeg, QDir(highRateOutput).filePath("song.ogg"), QDir(root).filePath("high-rate-delayed.pcm"));
    verifyAudioDelay(highRateOriginal, highRateDelayed, leadIn);
    require(readJson(QDir(highRateOutput).filePath("Info.dat")).value("_songApproximativeDuration").toDouble() == 0,
            "an unknown source duration is not converted to a misleading two-second song duration");
    for (int variant = 0; variant < 9; ++variant) {
        const QString variantProject = QDir(root).filePath(QString("protected-project-%1").arg(variant));
        require(ProjectStore::copyTree(project, variantProject, &error), error);
        const QString variantManifestPath = QDir(variantProject).filePath("project.lmsc");
        auto variantManifest = readJson(variantManifestPath);
        const QString variantMapPath = QDir(variantProject).filePath(variantManifest.value("assets").toString() + "/Expert.dat");
        auto variantMap = readJson(variantMapPath);
        if (variant == 0)
            variantMap.insert("_events", QJsonArray{QJsonObject{{"_time", 2}, {"_type", 100}, {"_floatValue", 180}}});
        else if (variant == 1)
            variantMap.insert("_events", QJsonArray{QJsonObject{{"_time", 2}, {"_type", 14}, {"_value", 1}}});
        else if (variant == 2)
            variantMap.insert("_customData", QJsonObject{{"_customEvents", QJsonArray{QJsonObject{{"_time", 1}, {"_type", "AnimateTrack"}}}}});
        else if (variant == 3)
            variantMap.insert("_notes", QJsonArray{QJsonObject{{"_time", 1}, {"_lineIndex", 3}, {"_lineLayer", 2}, {"_type", 0},
                                                            {"_cutDirection", 8}, {"customData", QJsonObject{{"animation", "keep"}}}}});
        else if (variant == 4)
            variantMap.insert("_obstacles", QJsonArray{QJsonObject{{"_time", 1}, {"_lineIndex", 0}, {"_type", 0}, {"_duration", 1}, {"_width", 5}}});
        else if (variant == 5)
            variantMap.insert("customData", "malformed mod data must not be silently discarded");
        else {
            auto variantInfo = variantManifest.value("info").toObject();
            const QJsonObject custom{{"keep", true}};
            if (variant == 6) variantInfo.insert("customData", custom);
            else {
                auto sets = variantInfo.value("_difficultyBeatmapSets").toArray();
                auto set = sets.first().toObject();
                if (variant == 7) set.insert("customData", custom);
                else {
                    auto difficulties = set.value("_difficultyBeatmaps").toArray();
                    auto difficulty = difficulties.first().toObject();
                    difficulty.insert("customData", custom); difficulties[0] = difficulty;
                    set.insert("_difficultyBeatmaps", difficulties);
                }
                sets[0] = set; variantInfo.insert("_difficultyBeatmapSets", sets);
            }
            variantManifest.insert("info", variantInfo);
        }
        require(ProjectStore::writeJson(variantMapPath, variantMap, &error), error);
        auto variantHashes = variantManifest.value("assetHashes").toObject();
        variantHashes.insert("Expert.dat", QString::fromLatin1(hash(variantMapPath).toHex())); variantManifest.insert("assetHashes", variantHashes);
        auto variantDifficulties = variantManifest.value("difficulties").toArray();
        auto variantDifficulty = variantDifficulties.first().toObject();
        variantDifficulty.insert("edits", QJsonArray{}); variantDifficulties[0] = variantDifficulty;
        variantManifest.insert("difficulties", variantDifficulties);
        require(ProjectStore::writeJson(variantManifestPath, variantManifest, &error), error);
        const auto protectedHashes = folderHashes(variantProject);
        BeatmapDocument protectedSong;
        require(protectedSong.loadProject(variantProject, &error), error);
        const QString output = QDir(root).filePath(QString("protected-output-%1").arg(variant));
        require(!SongExporter::exportSong(protectedSong, output, leadIn, tools, &error) && !QFileInfo::exists(output),
                "lead-in cannot bypass tempo, rotation, mod-data, or individual protected-object rules");
        const QString zeroOutput = QDir(root).filePath(QString("protected-zero-output-%1").arg(variant));
        require(SongExporter::exportSong(protectedSong, zeroOutput, 0, "missing-tools", &error) &&
                readJson(QDir(zeroOutput).filePath("Easy.dat")) == variantMap,
                "zero lead-in preserves protected raw content without requiring FFmpeg");
        auto expectedInfo = variantManifest.value("info").toObject();
        auto expectedSets = expectedInfo.value("_difficultyBeatmapSets").toArray();
        auto expectedSet = expectedSets.first().toObject();
        auto expectedDifficulties = expectedSet.value("_difficultyBeatmaps").toArray();
        auto expectedDifficulty = expectedDifficulties.first().toObject();
        expectedDifficulty.insert("_beatmapFilename", "Easy.dat"); expectedDifficulties[0] = expectedDifficulty;
        expectedSet.insert("_difficultyBeatmaps", expectedDifficulties); expectedSets[0] = expectedSet;
        expectedInfo.insert("_difficultyBeatmapSets", expectedSets);
        require(readJson(QDir(zeroOutput).filePath("Info.dat")) == expectedInfo, "zero lead-in retains protected Info/set/difficulty metadata");
        require(folderHashes(variantProject) == protectedHashes, "protected export attempts never mutate their original project");
    }
    require(folderHashes(project) == sourceHashes, "all export attempts leave the original project unchanged");
    verifyMultipleDifficulties(tools, root, project, leadIn, bpm, originalPcm);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    QTemporaryDir temporary;
    try {
        require(temporary.isValid(), "create temporary test workspace");
        runTests(QString::fromLocal8Bit(argv[1]), temporary.path());
        std::fprintf(stdout, "PASS single/multiple difficulty filenames, synchronized audio/all-map lead-in, stereo PCM, first-second notes, project immutability, repeat export, no-padding/import preservation, atomic errors, non-selected protected data\n");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
