#include "SongExporter.h"
#include "BeatmapDocument.h"
#include "ProjectStore.h"
#ifdef Q_OS_ANDROID
#include "NativeAudioTool.h"
#endif
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <cmath>

namespace lmsc {
namespace {
bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
bool hasCustomContent(const QJsonObject &object) {
    for (const auto &key : {QStringLiteral("_customData"), QStringLiteral("customData")})
        if (object.contains(key) && (!object.value(key).isObject() || !object.value(key).toObject().isEmpty())) return true;
    return false;
}

bool integer(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble())
        && std::floor(value.toDouble()) == value.toDouble();
}

bool shiftBasicMap(QJsonObject *map, double beats, QString *error) {
    const QStringList keys{QStringLiteral("_notes"), QStringLiteral("_obstacles"),
                           QStringLiteral("_events"), QStringLiteral("_waypoints")};
    if (map->value("_version").toString() != QStringLiteral("2.2.0"))
        return fail(error, QStringLiteral("开场缓冲仅支持新歌的基础 v2.2 谱面。"));
    for (auto it = map->begin(); it != map->end(); ++it)
        if (it.key() != QStringLiteral("_version") && !keys.contains(it.key()))
            return fail(error, QStringLiteral("谱面包含额外内容，不能自动后移；请将导出开场缓冲设为 0。"));
    for (const auto &key : keys) {
        if (!map->value(key).isArray()) return fail(error, QStringLiteral("谱面物件数组无效。"));
        QJsonArray shifted;
        for (const auto &value : map->value(key).toArray()) {
            if (!value.isObject() || !value.toObject().value("_time").isDouble())
                return fail(error, QStringLiteral("谱面物件时间无效，未完成导出。"));
            auto object = value.toObject();
            const double time = object.value("_time").toDouble();
            if (!std::isfinite(time) || time < 0 || !std::isfinite(time + beats))
                return fail(error, QStringLiteral("谱面物件时间无效，未完成导出。"));
            if (key == QStringLiteral("_events")) {
                const int type = object.value("_type").toInt(-1);
                if (type == 100 || type == 10 || type == 14 || type == 15)
                    return fail(error, QStringLiteral("谱面包含变速或旋转事件，不能自动后移；请将导出开场缓冲设为 0。"));
            }
            if (hasCustomContent(object))
                return fail(error, QStringLiteral("物件包含模组内容，不能自动后移；请将导出开场缓冲设为 0。"));
            if (key == QStringLiteral("_notes") || key == QStringLiteral("_obstacles")) {
                const int x = object.value("_lineIndex").toInt(-1);
                if (!integer(object.value("_lineIndex")) || x < 0 || x > 3)
                    return fail(error, QStringLiteral("物件含受保护的扩展坐标，不能自动后移；请将导出开场缓冲设为 0。"));
                const int type = object.value("_type").toInt(-1);
                if (!integer(object.value("_type")))
                    return fail(error, QStringLiteral("物件类型无效，不能自动后移；请将导出开场缓冲设为 0。"));
                if (key == QStringLiteral("_notes")) {
                    const int y = object.value("_lineLayer").toInt(-1);
                    const int cut = object.value("_cutDirection").toInt(-1);
                    if (!integer(object.value("_lineLayer")) || y < 0 || y > 2
                        || (type != 0 && type != 1 && type != 3)
                        || (type != 3 && (!integer(object.value("_cutDirection")) || cut < 0 || cut > 8)))
                        return fail(error, QStringLiteral("音符含受保护的扩展数据，不能自动后移；请将导出开场缓冲设为 0。"));
                } else {
                    const int width = object.value("_width").toInt(-1);
                    const double duration = object.value("_duration").toDouble(-1);
                    if ((type != 0 && type != 1) || !integer(object.value("_width"))
                        || width < 1 || x + width > 4 || !object.value("_duration").isDouble()
                        || !std::isfinite(duration) || duration <= 0)
                        return fail(error, QStringLiteral("墙含受保护的扩展数据，不能自动后移；请将导出开场缓冲设为 0。"));
                }
            }
            object.insert("_time", time + beats);
            shifted.append(object);
        }
        map->insert(key, shifted);
    }
    return true;
}
}

bool SongExporter::exportSong(const BeatmapDocument &document, const QString &destinationFolder,
                             double leadInSeconds, const QString &toolsDirectory, QString *error) {
    if (!std::isfinite(leadInSeconds) || leadInSeconds < 0 || leadInSeconds > 10)
        return fail(error, QStringLiteral("导出开场缓冲必须在 0～10 秒之间。"));
    if (!document.isNewSong() || leadInSeconds == 0)
        return document.exportSong(destinationFolder, error);
    const QFileInfo target(destinationFolder);
    if (target.exists()) return fail(error, QStringLiteral("导出目录必须是新目录，未覆盖已有歌曲。"));
    if (!document.isLoaded() || document.difficulties().isEmpty() || !document.readOnlyReason().isEmpty()
        || !document.timeMap().changes().isEmpty())
        return fail(error, QStringLiteral("该工程不支持新歌开场缓冲，未完成导出。"));
    for (const auto &object : document.objects())
        if (object.isProtected())
            return fail(error, QStringLiteral("曲谱含受保护物件，不能自动后移；请将导出开场缓冲设为 0。"));
#ifdef Q_OS_ANDROID
    Q_UNUSED(toolsDirectory)
    if (!NativeAudioTool::available())
        return fail(error, QStringLiteral("APK 缺少可用的 Android 音频组件，无法添加开场缓冲。"));
#else
    const QString ffmpeg = QDir(toolsDirectory).filePath(QStringLiteral("ffmpeg.exe"));
    if (toolsDirectory.isEmpty() || !QFileInfo(ffmpeg).isFile())
        return fail(error, QStringLiteral("缺少随软件提供的音频组件，无法添加开场缓冲。"));
#endif
    const QString parent = target.absolutePath();
    if (!QDir().mkpath(parent)) return fail(error, QStringLiteral("无法建立导出父目录。"));
    QTemporaryDir staging(QDir(parent).filePath(QStringLiteral(".lmsc-buffer-XXXXXX")));
    if (!staging.isValid()) return fail(error, QStringLiteral("无法建立导出暂存目录。"));
    const QString song = QDir(staging.path()).filePath(QStringLiteral("song"));
    if (!document.exportSong(song, error)) return false;
    QJsonObject info;
    const QString infoPath = QDir(song).filePath(QStringLiteral("Info.dat"));
    if (!ProjectStore::readJson(infoPath, &info, error)) return false;
    const auto sets = info.value("_difficultyBeatmapSets").toArray();
    if (sets.size() != 1 || sets.first().toObject().value("_beatmapCharacteristicName").toString() != "Standard"
        || sets.first().toObject().value("_difficultyBeatmaps").toArray().isEmpty() || hasCustomContent(info))
        return fail(error, QStringLiteral("开场缓冲仅支持基础 Standard 新歌，未完成导出。"));
    if (hasCustomContent(sets.first().toObject()))
        return fail(error, QStringLiteral("难度包含模组内容，不能自动后移；请将导出开场缓冲设为 0。"));
    const QString audioFile = info.value("_songFilename").toString();
    if (!ProjectStore::safeRelativePath(audioFile))
        return fail(error, QStringLiteral("导出资源路径无效。"));
    const qint64 delaySamples = qRound64(leadInSeconds * 44100);
    const double actualDelay = delaySamples / 44100.0;
    const double bpm = info.value("_beatsPerMinute").toDouble();
    if (!std::isfinite(bpm) || bpm <= 0) return fail(error, QStringLiteral("导出 BPM 无效。"));
    QVector<QPair<QString, QJsonObject>> maps;
    QSet<QString> filenames;
    for (const auto &value : sets.first().toObject().value("_difficultyBeatmaps").toArray()) {
        if (!value.isObject() || hasCustomContent(value.toObject()))
            return fail(error, QStringLiteral("难度包含模组内容，不能自动后移；请将导出开场缓冲设为 0。"));
        const QString mapFile = value.toObject().value("_beatmapFilename").toString();
        const QString key = mapFile.toLower();
        if (!ProjectStore::safeRelativePath(mapFile) || filenames.contains(key)
            || key == audioFile.toLower() || key == QStringLiteral("info.dat"))
            return fail(error, QStringLiteral("导出难度资源路径无效或重复。"));
        filenames.insert(key);
        QJsonObject map;
        const QString mapPath = QDir(song).filePath(mapFile);
        if (!ProjectStore::readJson(mapPath, &map, error) || !shiftBasicMap(&map, actualDelay * bpm / 60, error)) return false;
        maps.append(qMakePair(mapPath, map));
    }
    const QString originalAudio = QDir(song).filePath(audioFile);
    const QString paddedAudio = QDir(staging.path()).filePath(QStringLiteral("padded.ogg"));
    const QStringList arguments{QStringLiteral("-hide_banner"), QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-nostdin"), QStringLiteral("-n"), QStringLiteral("-i"), originalAudio,
        QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-vn"), QStringLiteral("-sn"), QStringLiteral("-dn"),
        QStringLiteral("-map_metadata"), QStringLiteral("-1"), QStringLiteral("-ar"), QStringLiteral("44100"),
        QStringLiteral("-af"), QStringLiteral("aresample=44100,adelay=delays=%1S:all=1").arg(delaySamples),
        QStringLiteral("-c:a"), QStringLiteral("libvorbis"), QStringLiteral("-q:a"), QStringLiteral("5"), paddedAudio};
#ifdef Q_OS_ANDROID
    const auto audioResult = NativeAudioTool::run(false, arguments, {}, {}, 180000);
    if (audioResult.timedOut) return fail(error, QStringLiteral("添加开场缓冲超时，未完成导出。"));
    if (audioResult.exitCode != 0 || QFileInfo(paddedAudio).size() <= 0)
        return fail(error, QStringLiteral("添加开场缓冲失败：%1").arg(audioResult.error));
#else
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; });
#endif
    process.setStandardOutputFile(QProcess::nullDevice());
    const QString logPath = QDir(staging.path()).filePath(QStringLiteral("ffmpeg.log"));
    process.setStandardErrorFile(logPath);
    process.start(ffmpeg, arguments);
    if (!process.waitForStarted(10000)) return fail(error, QStringLiteral("音频组件无法启动：%1").arg(process.errorString()));
    if (!process.waitForFinished(180000)) {
        process.kill(); process.waitForFinished(3000);
        return fail(error, QStringLiteral("添加开场缓冲超时，未完成导出。"));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || QFileInfo(paddedAudio).size() <= 0) {
        QFile log(logPath); log.open(QIODevice::ReadOnly);
        log.seek(qMax<qint64>(0, log.size() - 3000));
        return fail(error, QStringLiteral("添加开场缓冲失败：%1").arg(QString::fromUtf8(log.readAll()).trimmed()));
    }
#endif
    if (!QFile::remove(originalAudio) || !QFile::rename(paddedAudio, originalAudio))
        return fail(error, QStringLiteral("无法完成导出音频替换。"));
    info.insert("_previewStartTime", info.value("_previewStartTime").toDouble() + actualDelay);
    if (info.value("_songApproximativeDuration").toDouble() > 0)
        info.insert("_songApproximativeDuration", info.value("_songApproximativeDuration").toDouble() + actualDelay);
    for (const auto &map : maps)
        if (!ProjectStore::writeJson(map.first, map.second, error)) return false;
    if (!ProjectStore::writeJson(infoPath, info, error)) return false;
    if (!QDir().rename(song, target.absoluteFilePath()))
        return fail(error, QStringLiteral("无法完成导出目录提交；目标可能已被创建。"));
    return true;
}
}
