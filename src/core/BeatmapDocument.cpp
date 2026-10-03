#include "BeatmapDocument.h"
#include "ProjectStore.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace lmsc {
namespace {
bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
bool sameFields(const BeatObject &a, const BeatObject &b) {
    return a.kind == b.kind && a.beat == b.beat && a.x == b.x && a.y == b.y &&
           a.color == b.color && a.direction == b.direction && a.duration == b.duration &&
           a.width == b.width && a.height == b.height;
}
bool finite(double value) { return std::isfinite(value); }
bool integral(const QJsonValue &value) {
    return value.isDouble() && finite(value.toDouble()) && value.toDouble() == std::floor(value.toDouble()) &&
           std::abs(value.toDouble()) < 10000000.0;
}
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject emptyNewSongMap() {
    return {{"_version", "2.2.0"}, {"_notes", QJsonArray{}}, {"_obstacles", QJsonArray{}},
            {"_events", QJsonArray{}}, {"_waypoints", QJsonArray{}}};
}
QString cell(double beat, int x, int y) {
    if (!finite(beat) || std::abs(beat) > 10000000.0)
        return QStringLiteral("invalid/%1/%2/%3").arg(QString::number(beat, 'g', 17)).arg(x).arg(y);
    return QStringLiteral("%1/%2/%3").arg(qRound64(beat * 1000000.0)).arg(x).arg(y);
}
int mirroredDirection(int direction) {
    static const int mirror[9] = {0, 1, 3, 2, 5, 4, 7, 6, 8};
    return direction >= 0 && direction <= 8 ? mirror[direction] : direction;
}
QJsonObject objectJson(const BeatObject &object) {
    return {{"id", object.id}, {"kind", int(object.kind)}, {"beat", object.beat}, {"x", object.x},
            {"y", object.y}, {"color", object.color}, {"direction", object.direction},
            {"duration", object.duration}, {"width", object.width}, {"height", object.height}};
}
BeatObject objectFromJson(const QJsonObject &json) {
    BeatObject object;
    object.id = json.value("id").toString();
    object.kind = ObjectKind(json.value("kind").toInt(-1));
    object.beat = json.value("beat").toDouble(-1);
    object.x = json.value("x").toInt(-1);
    object.y = json.value("y").toInt(-1);
    object.color = json.value("color").toInt();
    object.direction = json.value("direction").toInt(8);
    object.duration = json.value("duration").toDouble(1);
    object.width = json.value("width").toInt(1);
    object.height = json.value("height").toInt(5);
    return object;
}
QString normalizedManifest(const QString &path) {
    if (QFileInfo(path).isDir() || QFileInfo(path).suffix().isEmpty()) return QDir(path).filePath("project.lmsc");
    return QFileInfo(path).absoluteFilePath();
}
QString timingExtension(const QJsonObject &json, const QString &path, bool editorCustomData = false) {
    for (auto it = json.begin(); it != json.end(); ++it) {
        const QString key = it.key().toLower();
        if (editorCustomData && it.key() == QStringLiteral("bookmarksUseOfficialBpmEvents")) {
            if (!it.value().isBool())
                return QStringLiteral("%1.%2 应为布尔类型，实际类型无法可靠解释。")
                        .arg(path, it.key());
            continue;
        }
        if ((key.contains("bpm") || key.contains("timescale") || key.contains("timeoffset") ||
             key.contains("tempo")) && !it.value().isNull()) {
            if (!it.value().isArray() || !it.value().toArray().isEmpty())
                return QStringLiteral("%1.%2 含尚未支持的时间扩展，无法可靠换算秒与拍。")
                        .arg(path, it.key());
        }
    }
    return {};
}
QJsonObject recognizedEditorTiming(QJsonObject custom, double baseBpm) {
    // ChroMapper uses this boolean only to choose the BPM source for its editor
    // bookmarks. It does not retime gameplay objects; keep the original JSON.
    if (custom.value("bookmarksUseOfficialBpmEvents").isBool())
        custom.remove("bookmarksUseOfficialBpmEvents");
    // Old MMA editor grid markers do not retime the map when every marker uses
    // the unchanged base BPM. Keep the original JSON, but do not lock this case.
    for (const auto &key : {QStringLiteral("_BPMChanges"), QStringLiteral("_bpmChanges")}) {
        if (!custom.value(key).isArray()) continue;
        bool harmless = true;
        const QStringList allowed{"_time", "_BPM", "_beatsPerBar", "_metronomeOffset"};
        for (const auto &value : custom.value(key).toArray()) {
            const auto marker = value.toObject();
            if (marker.isEmpty() || !marker.value("_BPM").isDouble() || !marker.value("_time").isDouble() ||
                !finite(marker.value("_time").toDouble()) || marker.value("_time").toDouble() < 0.0 ||
                marker.value("_BPM").toDouble() != baseBpm) { harmless = false; break; }
            for (auto it = marker.begin(); it != marker.end(); ++it)
                if (!allowed.contains(it.key()) || !it.value().isDouble()) { harmless = false; break; }
        }
        if (harmless) custom.remove(key);
    }
    return custom;
}
}

struct BeatmapDocument::Impl {
    struct Entry {
        BeatObject original;
        BeatObject object;
        QString array;
        int index = -1;
        bool deleted = false;
    };
    struct Snapshot { BeatObject object; bool deleted = false; };
    struct Change { int entry; Snapshot before, after; };
    struct DifficultySnapshot { QString name; int rank = 7; };
    struct Command {
        QVector<Change> changes;
        quint64 beforeKey, afterKey;
        bool difficultyChanged = false;
        DifficultySnapshot beforeDifficulty, afterDifficulty;
    };
    struct Track {
        Difficulty descriptor;
        QJsonObject raw;
        QVector<Entry> entries;
        QHash<QString, int> byId;
        QSet<QString> endpoints;
        TimeMap time;
        QString readOnly;
        QVector<Command> history;
        int cursor = 0;
        quint64 stateKey = 0, savedKey = 0;
        bool v3 = false;
        bool active = true, savedActive = true, generated = false;
    };
    struct SongCommand { int track, beforeCurrent; Command edit; bool added = false; };
    QVector<SongCommand> songHistory;
    int songCursor = 0;
    std::unique_ptr<QTemporaryDir> temporary;
    QString assets;
    QString infoRelative;
    QString audioRelative;
    QString coverRelative;
    QString manifest;
    QJsonObject info;
    QJsonObject assetHashes;
    QString sourceRoot;
    QJsonObject source;
    QVector<Track> tracks;
    QVector<Difficulty> descriptors;
    QVector<BeatObject> view;
    int current = -1;
    bool newSong = false;
    double newFirstBeatSeconds = 0.0;
    QStringList warnings;
    quint64 nextKey = 1, metaKey = 0, savedMetaKey = 0;
    quint64 revision = 0;

    Track *track() { return current >= 0 && current < tracks.size() ? &tracks[current] : nullptr; }
    const Track *track() const { return current >= 0 && current < tracks.size() ? &tracks[current] : nullptr; }
    void refresh() {
        view.clear();
        if (const auto *selected = track()) {
            view.reserve(selected->entries.size());
            for (const auto &entry : selected->entries) if (!entry.deleted) view.append(entry.object);
            std::stable_sort(view.begin(), view.end(), [](const BeatObject &a, const BeatObject &b) { return a.beat < b.beat; });
        }
        descriptors.clear();
        for (const auto &selected : tracks) if (selected.active) descriptors.append(selected.descriptor);
    }
    QString absolute(const QString &relative) const { return QDir(assets).filePath(relative); }
    QString actualRelative(const QString &reference) const {
        if (!ProjectStore::safeRelativePath(reference)) return {};
        const QString candidate = QDir::cleanPath(QDir(QFileInfo(infoRelative).path()).filePath(reference));
        if (!ProjectStore::safeRelativePath(candidate)) return {};
        if (QFileInfo(absolute(candidate)).isFile()) return candidate;
        const auto list = ProjectStore::files(assets);
        QString match;
        for (const auto &path : list) if (path.compare(candidate, Qt::CaseInsensitive) == 0) {
            if (!match.isEmpty()) return {};
            match = path;
        }
        return match;
    }
    bool initialize(QString *error);
    bool parseTrack(Track *track, QString *error);
    bool validate(const Track &track, const QVector<BeatObject> &objects,
                  const QSet<QString> &replaced, QString *error) const;
    bool commit(Track *track, QVector<Change> changes, QString *error,
                const QString &difficultyName = {}, int difficultyRank = 0,
                bool added = false, int beforeCurrent = -1);
    void applyDifficulty(Track *track, const DifficultySnapshot &snapshot);
    void syncNewSongDifficulties();
    QJsonObject merged(const Track &track) const;
    bool changed(const Track &track) const;
    QJsonObject state() const;
    bool restoreState(const QJsonObject &state, QString *error);
};

bool BeatmapDocument::Impl::initialize(QString *error) {
    QString listError;
    const auto list = ProjectStore::files(assets, &listError);
    if (!listError.isEmpty()) return fail(error, listError);
    for (const auto &relative : list) {
        QFile asset(absolute(relative));
        if (!asset.open(QIODevice::ReadOnly)) return fail(error, asset.errorString());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&asset)) return fail(error, QStringLiteral("无法计算原始资源摘要。"));
        assetHashes.insert(relative, QString::fromLatin1(hash.result().toHex()));
    }
    QStringList candidates;
    for (const auto &path : list) if (QFileInfo(path).fileName().compare("Info.dat", Qt::CaseInsensitive) == 0) candidates.append(path);
    if (candidates.size() != 1) return fail(error, QStringLiteral("歌曲包需有且只有一份 Info.dat，实际找到 %1 份。").arg(candidates.size()));
    infoRelative = candidates.first();
    if (!ProjectStore::readJson(absolute(infoRelative), &info, error)) return false;
    const QString infoVersion = info.value("_version").toString(info.value("version").toString());
    if (!infoVersion.startsWith("2.")) return fail(error, QStringLiteral("首版支持 Info v2；此包为 %1，未转换或覆盖原文件。").arg(infoVersion));
    const QString reference = info.value("_songFilename").toString();
    audioRelative = actualRelative(reference);
    if (audioRelative.isEmpty() && QFileInfo(reference).suffix().compare("egg", Qt::CaseInsensitive) == 0) {
        const QString sibling = QFileInfo(reference).path() + '/' + QFileInfo(reference).completeBaseName() + ".ogg";
        audioRelative = actualRelative(QDir::cleanPath(sibling));
        if (audioRelative.isEmpty()) {
            QStringList oggs;
            const QString songParent = QFileInfo(infoRelative).path();
            for (const auto &path : list)
                if (QFileInfo(path).suffix().compare("ogg", Qt::CaseInsensitive) == 0 && QFileInfo(path).path() == songParent) oggs.append(path);
            if (oggs.size() == 1) audioRelative = oggs.first();
        }
        if (!audioRelative.isEmpty()) warnings.append(QStringLiteral("Info 引用的 .egg 不存在，试听使用实际 .ogg；导出保留原引用和音频文件。"));
    }
    if (audioRelative.isEmpty()) return fail(error, QStringLiteral("找不到 Info 引用的音频，且不存在唯一可确认的兼容候选：%1").arg(reference));
    coverRelative = actualRelative(info.value("_coverImageFilename").toString());
    if (coverRelative.isEmpty()) warnings.append(QStringLiteral("封面引用不存在，原始元数据仍保留。"));
    const auto sets = info.value("_difficultyBeatmapSets").toArray();
    QSet<QString> filenames;
    for (const auto &setValue : sets) {
        const auto set = setValue.toObject();
        for (const auto &difficultyValue : set.value("_difficultyBeatmaps").toArray()) {
            const auto difficulty = difficultyValue.toObject();
            Track selected;
            selected.descriptor.id = QString::number(tracks.size());
            selected.descriptor.characteristic = set.value("_beatmapCharacteristicName").toString();
            selected.descriptor.name = difficulty.value("_difficulty").toString();
            selected.descriptor.rank = difficulty.value("_difficultyRank").toInt();
            selected.descriptor.filename = actualRelative(difficulty.value("_beatmapFilename").toString());
            if (selected.descriptor.filename.isEmpty()) return fail(error, QStringLiteral("找不到难度文件：%1").arg(difficulty.value("_beatmapFilename").toString()));
            const QString lower = selected.descriptor.filename.toLower();
            if (filenames.contains(lower)) return fail(error, QStringLiteral("多份难度引用同一文件，首版暂不编辑此包。"));
            filenames.insert(lower);
            if (!ProjectStore::readJson(absolute(selected.descriptor.filename), &selected.raw, error) || !parseTrack(&selected, error)) return false;
            tracks.append(std::move(selected));
        }
    }
    if (tracks.isEmpty()) return fail(error, QStringLiteral("Info 没有可打开的难度。"));
    current = 0;
    for (int i = 0; i < tracks.size(); ++i) if (tracks[i].descriptor.characteristic == "Standard") { current = i; break; }
    refresh();
    return true;
}

bool BeatmapDocument::Impl::parseTrack(Track *selected, QString *error) {
    auto &raw = selected->raw;
    selected->descriptor.version = raw.value("version").toString(raw.value("_version").toString());
    selected->v3 = selected->descriptor.version.startsWith("3.");
    if (!selected->v3 && !selected->descriptor.version.startsWith("2.")) {
        selected->readOnly = QStringLiteral("首版仅编辑谱面 v2/v3；此难度 %1 按原文件保留。").arg(selected->descriptor.version);
        selected->time.configure(info.value("_beatsPerMinute").toDouble(120), -info.value("_songTimeOffset").toDouble());
        return true;
    }
    if (selected->descriptor.characteristic != "Standard")
        selected->readOnly = QStringLiteral("首版仅编辑 Standard 双手玩法；其他玩法完整保留。" );
    if (info.value("_shuffle").toDouble() != 0.0)
        selected->readOnly = QStringLiteral("歌曲使用旧版 swing/shuffle 时间扩展，首版按原样保护。" );
    if ((info.contains("_songTimeOffset") && !info.value("_songTimeOffset").isDouble()) ||
        (info.contains("_shuffle") && !info.value("_shuffle").isDouble()))
        selected->readOnly = QStringLiteral("歌曲时间元数据不是可解释的数值，首版按原样保护。" );
    QVector<TempoChange> changes;
    const QString timingKey = selected->v3 ? "bpmEvents" : "_events";
    if (raw.contains(timingKey) && !raw.value(timingKey).isArray()) selected->readOnly = QStringLiteral("变速数组结构异常，无法可靠换算秒与拍。" );
    for (const auto &value : raw.value(timingKey).toArray()) {
        const auto event = value.toObject();
        if (selected->v3 || event.value("_type").toInt(-1) == 100) {
            const QString beatKey = selected->v3 ? "b" : "_time";
            const QString bpmKey = selected->v3 ? "m" : "_floatValue";
            if (!event.value(beatKey).isDouble() || !event.value(bpmKey).isDouble())
                selected->readOnly = QStringLiteral("BPM 事件缺少可解释的数值，难度受保护。" );
            changes.append({event.value(beatKey).toDouble(-1), event.value(bpmKey).toDouble(-1)});
        } else if (event.value("_type").toInt() == 10 || event.value("_type").toInt() == 14 || event.value("_type").toInt() == 15) {
            // Type 14/15 are rotation, not ordinary flat-track editing.
            if (event.value("_type").toInt() == 10)
                selected->readOnly = QStringLiteral("谱面含旧版 type 10 变速事件，首版无法可靠解释并按原样保护。" );
            else if (event.value("_type").toInt() == 14 || event.value("_type").toInt() == 15)
                selected->readOnly = QStringLiteral("谱面含旋转轨道事件，首版按原样保留并禁止基础编辑。" );
        }
    }
    QString timeError;
    if (!selected->time.configure(info.value("_beatsPerMinute").toDouble(-1),
                                  -info.value("_songTimeOffset").toDouble(), changes, &timeError)) {
        selected->readOnly = timeError;
        selected->time.configure(120, 0);
    }
    const double baseBpm = info.value("_beatsPerMinute").toDouble();
    QJsonObject otherTiming = raw;
    if (selected->v3) otherTiming.remove("bpmEvents");
    QString timingReason;
    const QString mapPath = selected->descriptor.filename;
    const auto inspectTiming = [&timingReason](const QJsonObject &json, const QString &path, bool custom = false) {
        if (timingReason.isEmpty()) timingReason = timingExtension(json, path, custom);
    };
    inspectTiming(recognizedEditorTiming(raw.value("_customData").toObject(), baseBpm), mapPath + "._customData", true);
    inspectTiming(recognizedEditorTiming(raw.value("customData").toObject(), baseBpm), mapPath + ".customData", true);
    inspectTiming(otherTiming, mapPath);
    inspectTiming(recognizedEditorTiming(info.value("_customData").toObject(), baseBpm), infoRelative + "._customData", true);
    if (!timingReason.isEmpty())
        selected->readOnly = timingReason + QStringLiteral(" 为保护原数据，此难度设为只读；另存工程会原样保留该字段，不会解除保护。");
    if (!raw.value("rotationEvents").toArray().isEmpty())
        selected->readOnly = QStringLiteral("谱面含旋转轨道事件，首版按原样保留并禁止基础编辑。" );

    auto addEndpoint = [selected](const QJsonObject &arc, bool tail, bool old) {
        const QString bk = old ? (tail ? "_tailTime" : "_headTime") : (tail ? "tb" : "b");
        const QString xk = old ? (tail ? "_tailLineIndex" : "_headLineIndex") : (tail ? "tx" : "x");
        const QString yk = old ? (tail ? "_tailLineLayer" : "_headLineLayer") : (tail ? "ty" : "y");
        if (arc.value(bk).isDouble() && finite(arc.value(bk).toDouble()) && arc.value(bk).toDouble() >= 0 &&
            arc.value(bk).toDouble() <= 10000000.0 && integral(arc.value(xk)) && integral(arc.value(yk)))
            selected->endpoints.insert(cell(arc.value(bk).toDouble(), arc.value(xk).toInt(), arc.value(yk).toInt()));
        else selected->readOnly = QStringLiteral("弧线或链条端点无法可靠解释，难度受保护。" );
    };
    for (const QString &key : {QStringLiteral("sliders"), QStringLiteral("burstSliders"), QStringLiteral("_sliders")}) {
        if (raw.contains(key) && !raw.value(key).isArray()) selected->readOnly = QStringLiteral("高级物件数组结构异常，难度受保护。" );
        for (const auto &value : raw.value(key).toArray()) {
            addEndpoint(value.toObject(), false, key.startsWith('_'));
            addEndpoint(value.toObject(), true, key.startsWith('_'));
        }
    }
    const QStringList arrays = selected->v3 ? QStringList{"colorNotes", "bombNotes", "obstacles"} : QStringList{"_notes", "_obstacles"};
    for (const auto &key : arrays) {
        if (raw.contains(key) && !raw.value(key).isArray()) {
            selected->readOnly = QStringLiteral("基础物件数组结构异常，难度受保护。" );
            continue;
        }
        const auto array = raw.value(key).toArray();
        for (int index = 0; index < array.size(); ++index) {
            const auto json = array[index].toObject();
            Entry entry;
            entry.array = key;
            entry.index = index;
            auto &object = entry.object;
            object.id = selected->descriptor.id + ':' + key + ':' + QString::number(index);
            const bool wall = key == "obstacles" || key == "_obstacles";
            object.kind = wall ? ObjectKind::Wall : (key == "bombNotes" || json.value("_type").toInt() == 3 ? ObjectKind::Bomb : ObjectKind::Note);
            const QString bk = selected->v3 ? "b" : "_time";
            const QString xk = selected->v3 ? "x" : "_lineIndex";
            const QString yk = selected->v3 ? "y" : "_lineLayer";
            object.beat = json.value(bk).toDouble(-1);
            object.x = json.value(xk).toInt(-1);
            object.y = json.value(yk).toInt(wall ? 0 : -1);
            object.color = selected->v3 ? json.value("c").toInt() : json.value("_type").toInt();
            object.direction = selected->v3 ? json.value("d").toInt(8) : json.value("_cutDirection").toInt(8);
            if (object.kind == ObjectKind::Bomb) { object.color = 0; object.direction = 8; }
            if (wall) {
                object.duration = json.value(selected->v3 ? "d" : "_duration").toDouble(-1);
                object.width = json.value(selected->v3 ? "w" : "_width").toInt(-1);
                object.height = json.value(selected->v3 ? "h" : "_height").toInt(5);
                if (!selected->v3) {
                    const int type = json.value("_type").toInt(-1);
                    if (type == 0) { object.y = 0; object.height = 5; }
                    else if (type == 1) { object.y = 2; object.height = 3; }
                    else if (type != 2) object.protectedReason = QStringLiteral("扩展墙类型，首版不修改。" );
                }
                object.color = 0;
                object.direction = 8;
            }
            if (!json.value(bk).isDouble() || !integral(json.value(xk)) ||
                (!wall && !integral(json.value(yk))) || (!wall && !selected->v3 && !integral(json.value("_type"))))
                object.protectedReason = QStringLiteral("物件含异常或扩展坐标/类型。" );
            if (object.kind == ObjectKind::Note && (!integral(json.value(selected->v3 ? "c" : "_type")) ||
                                                  !integral(json.value(selected->v3 ? "d" : "_cutDirection"))))
                object.protectedReason = QStringLiteral("音符颜色或方向无法可靠解释。" );
            if (wall && (!integral(json.value(selected->v3 ? "w" : "_width")) ||
                         (selected->v3 && (!integral(json.value("h")) || !integral(json.value("y"))))))
                object.protectedReason = QStringLiteral("墙含异常或扩展尺寸。" );
            if (!json.value("_customData").toObject().isEmpty() || !json.value("customData").toObject().isEmpty())
                object.protectedReason = QStringLiteral("物件带模组或动画数据，首版保留并保护。" );
            if (selected->v3 && object.kind == ObjectKind::Note && json.value("a").toDouble() != 0.0)
                object.protectedReason = QStringLiteral("音符使用扩展切割角度，首版保护。" );
            if (!wall && selected->endpoints.contains(cell(object.beat, object.x, object.y)))
                object.protectedReason = QStringLiteral("音符关联弧线或链条端点，首版保护。" );
            if (!finite(object.beat) || object.beat < 0 || object.beat > 10000000.0 || object.x < 0 || object.x > 3 || object.y < 0 || object.y > 2 ||
                (object.kind == ObjectKind::Note && (object.color < 0 || object.color > 1 || object.direction < 0 || object.direction > 8)) ||
                (wall && (!finite(object.duration) || object.duration <= 0 || object.width < 1 || object.x + object.width > 4 ||
                          object.height < 1 || object.y + object.height > 5)))
                object.protectedReason = QStringLiteral("物件超出首版基础网格或尺寸，原样保护。" );
            if (!selected->readOnly.isEmpty()) object.protectedReason = selected->readOnly;
            entry.original = object;
            selected->byId.insert(object.id, selected->entries.size());
            selected->entries.append(entry);
        }
    }
    Q_UNUSED(error)
    return true;
}

bool BeatmapDocument::Impl::validate(const Track &selected, const QVector<BeatObject> &objects,
                                    const QSet<QString> &replaced, QString *error) const {
    if (!selected.readOnly.isEmpty()) return fail(error, selected.readOnly);
    QSet<QString> occupied;
    for (const auto &entry : selected.entries)
        if (!entry.deleted && entry.object.kind != ObjectKind::Wall && !replaced.contains(entry.object.id))
            occupied.insert(cell(entry.object.beat, entry.object.x, entry.object.y));
    for (const auto &object : objects) {
        if (object.isProtected()) return fail(error, object.protectedReason);
        if (!finite(object.beat) || object.beat < 0 || object.beat > 10000000.0 || object.x < 0 || object.x > 3 || object.y < 0 || object.y > 2)
            return fail(error, QStringLiteral("拍数必须非负，位置必须在 4×3 网格内。"));
        if (object.kind == ObjectKind::Note) {
            if (object.color < 0 || object.color > 1 || object.direction < 0 || object.direction > 8)
                return fail(error, QStringLiteral("普通音符的颜色或方向无效。"));
        } else if (object.kind == ObjectKind::Wall) {
            if (!finite(object.duration) || object.duration <= 0 || object.width < 1 || object.x + object.width > 4 ||
                object.height < 1 || object.y + object.height > 5)
                return fail(error, QStringLiteral("墙的持续拍数、宽度或高度无效。"));
            if (!selected.v3 && !((object.y == 0 && object.height == 5) || (object.y == 2 && object.height == 3)) &&
                !selected.descriptor.version.startsWith("2.6"))
                return fail(error, QStringLiteral("此 v2 谱仅支持全高墙（y=0，高5）或蹲墙（y=2，高3）。"));
        } else if (object.kind != ObjectKind::Bomb) return fail(error, QStringLiteral("未知编辑物件类型。"));
        if (object.kind != ObjectKind::Wall) {
            const QString target = cell(object.beat, object.x, object.y);
            if (selected.endpoints.contains(target)) return fail(error, QStringLiteral("目标位置关联弧线或链条，操作未执行。"));
            if (occupied.contains(target)) return fail(error, QStringLiteral("目标拍数与格子已有物件，整批操作未执行。"));
            occupied.insert(target);
        }
    }
    return true;
}

void BeatmapDocument::Impl::applyDifficulty(Track *selected, const DifficultySnapshot &snapshot) {
    selected->descriptor.name = snapshot.name;
    selected->descriptor.rank = snapshot.rank;
    syncNewSongDifficulties();
}

void BeatmapDocument::Impl::syncNewSongDifficulties() {
    auto sets = info.value("_difficultyBeatmapSets").toArray();
    auto set = sets.first().toObject();
    const auto previous = set.value("_difficultyBeatmaps").toArray();
    QJsonArray difficulties;
    for (const auto &selected : tracks) if (selected.active) {
        QJsonObject difficulty;
        for (const auto &value : previous)
            if (value.toObject().value("_beatmapFilename").toString() == selected.descriptor.filename) {
                difficulty = value.toObject(); break;
            }
        if (difficulty.isEmpty()) difficulty = {{"_noteJumpMovementSpeed",12},{"_noteJumpStartBeatOffset",0}};
        difficulty.insert("_difficulty", selected.descriptor.name);
        difficulty.insert("_difficultyRank", selected.descriptor.rank);
        difficulty.insert("_beatmapFilename", selected.descriptor.filename);
        difficulties.append(difficulty);
    }
    set.insert("_difficultyBeatmaps", difficulties);
    sets[0] = set;
    info.insert("_difficultyBeatmapSets", sets);
}

bool BeatmapDocument::Impl::commit(Track *selected, QVector<Change> changes, QString *error,
                                   const QString &difficultyName, int difficultyRank, bool added, int beforeCurrent) {
    const bool difficultyChanged = !difficultyName.isEmpty()
        && (difficultyName != selected->descriptor.name || difficultyRank != selected->descriptor.rank);
    if (changes.isEmpty() && !difficultyChanged && !added) return true;
    const qint64 bytes = qint64(changes.size()) * sizeof(Change);
    if (bytes > 16 * 1024 * 1024) return fail(error, QStringLiteral("本次批量操作超过撤销记录上限，请分段编辑。"));
    while (selected->history.size() > selected->cursor) selected->history.removeLast();
    Command command{std::move(changes), selected->stateKey, nextKey++};
    command.difficultyChanged = difficultyChanged;
    if (difficultyChanged) {
        command.beforeDifficulty = {selected->descriptor.name, selected->descriptor.rank};
        command.afterDifficulty = {difficultyName, difficultyRank};
        applyDifficulty(selected, command.afterDifficulty);
    }
    for (const auto &change : command.changes) {
        selected->entries[change.entry].object = change.after.object;
        selected->entries[change.entry].deleted = change.after.deleted;
    }
    selected->stateKey = command.afterKey;
    if (newSong) {
        while (songHistory.size() > songCursor) songHistory.removeLast();
        songHistory.append({int(selected - tracks.data()), beforeCurrent < 0 ? current : beforeCurrent,
                            std::move(command), added});
        songCursor = songHistory.size();
        qint64 total = 0;
        for (const auto &item : songHistory) total += qint64(item.edit.changes.size()) * sizeof(Change);
        while (songHistory.size() > 256 || total > 32 * 1024 * 1024) {
            total -= qint64(songHistory.first().edit.changes.size()) * sizeof(Change);
            songHistory.removeFirst(); --songCursor;
        }
        if (added) syncNewSongDifficulties();
        refresh(); ++revision; return true;
    }
    selected->history.append(std::move(command));
    selected->cursor = selected->history.size();
    qint64 total = 0;
    for (const auto &item : selected->history) total += qint64(item.changes.size()) * sizeof(Change);
    while (selected->history.size() > 256 || total > 32 * 1024 * 1024) {
        total -= qint64(selected->history.first().changes.size()) * sizeof(Change);
        selected->history.removeFirst();
        --selected->cursor;
    }
    qint64 combined = 0;
    for (const auto &difficulty : tracks)
        for (const auto &item : difficulty.history) combined += qint64(item.changes.size()) * sizeof(Change);
    while (combined > 32 * 1024 * 1024) {
        Track *oldest = nullptr;
        for (auto &difficulty : tracks)
            if (!difficulty.history.isEmpty() && (!oldest || difficulty.history.first().afterKey < oldest->history.first().afterKey)) oldest = &difficulty;
        if (!oldest) break;
        if (oldest->cursor == 0) {
            // A redo chain depends on its first command; discard it together.
            for (const auto &item : oldest->history) combined -= qint64(item.changes.size()) * sizeof(Change);
            oldest->history.clear();
        } else {
            combined -= qint64(oldest->history.first().changes.size()) * sizeof(Change);
            oldest->history.removeFirst();
            --oldest->cursor;
        }
    }
    refresh();
    ++revision;
    return true;
}

bool BeatmapDocument::Impl::changed(const Track &selected) const {
    for (const auto &entry : selected.entries)
        if ((entry.index < 0 && !entry.deleted) || (entry.index >= 0 && (entry.deleted || !sameFields(entry.object, entry.original)))) return true;
    return false;
}

QJsonObject BeatmapDocument::Impl::merged(const Track &selected) const {
    QJsonObject result = selected.raw;
    const QStringList arrays = selected.v3 ? QStringList{"colorNotes", "bombNotes", "obstacles"} : QStringList{"_notes", "_obstacles"};
    for (const auto &key : arrays) {
        QHash<int, const Entry *> existing;
        QVector<const Entry *> additions;
        bool touched = false;
        for (const auto &entry : selected.entries) if (entry.array == key) {
            if (entry.index >= 0) existing.insert(entry.index, &entry);
            else if (!entry.deleted) additions.append(&entry);
            if ((entry.index >= 0 && (entry.deleted || !sameFields(entry.object, entry.original))) || (entry.index < 0 && !entry.deleted)) touched = true;
        }
        if (!touched) continue;
        auto encode = [this, &selected](QJsonObject rawObject, const Entry &entry) {
            const auto &o = entry.object;
            const auto &before = entry.original;
            const bool added = entry.index < 0;
            auto put = [&rawObject, added](const QString &field, double value, double original) {
                if (added || value != original) rawObject.insert(field, value);
            };
            const double exportedBeat = newSong ? o.beat + newFirstBeatSeconds * info.value("_beatsPerMinute").toDouble() / 60.0 : o.beat;
            put(selected.v3 ? "b" : "_time", exportedBeat, before.beat);
            put(selected.v3 ? "x" : "_lineIndex", o.x, before.x);
            if (o.kind == ObjectKind::Wall) {
                put(selected.v3 ? "d" : "_duration", o.duration, before.duration);
                put(selected.v3 ? "w" : "_width", o.width, before.width);
                if (selected.v3) { put("y", o.y, before.y); put("h", o.height, before.height); }
                else if (added || o.y != before.y || o.height != before.height) {
                    if (o.y == 0 && o.height == 5) rawObject.insert("_type", 0);
                    else if (o.y == 2 && o.height == 3) rawObject.insert("_type", 1);
                    else { rawObject.insert("_type", 2); rawObject.insert("_lineLayer", o.y); rawObject.insert("_height", o.height); }
                }
            } else {
                put(selected.v3 ? "y" : "_lineLayer", o.y, before.y);
                if (o.kind == ObjectKind::Note) {
                    put(selected.v3 ? "c" : "_type", o.color, before.color);
                    put(selected.v3 ? "d" : "_cutDirection", o.direction, before.direction);
                    if (added && selected.v3) rawObject.insert("a", 0);
                } else if (added && !selected.v3) { rawObject.insert("_type", 3); rawObject.insert("_cutDirection", 8); }
            }
            return rawObject;
        };
        QJsonArray output;
        const auto original = selected.raw.value(key).toArray();
        for (int i = 0; i < original.size(); ++i) {
            const Entry *entry = existing.value(i, nullptr);
            if (!entry) { output.append(original[i]); continue; }
            if (!entry->deleted) output.append(encode(original[i].toObject(), *entry));
        }
        for (const auto *entry : additions) output.append(encode({}, *entry));
        if (newSong) {
            QVector<QJsonObject> sorted;
            sorted.reserve(output.size());
            for (const auto &value : output) sorted.append(value.toObject());
            const QString beatKey = selected.v3 ? "b" : "_time";
            std::stable_sort(sorted.begin(), sorted.end(), [&beatKey](const QJsonObject &a, const QJsonObject &b) {
                return a.value(beatKey).toDouble() < b.value(beatKey).toDouble();
            });
            output = {};
            for (const auto &value : sorted) output.append(value);
        }
        result.insert(key, output);
    }
    return result;
}

QJsonObject BeatmapDocument::Impl::state() const {
    QJsonArray difficulties;
    for (const auto &selected : tracks) if (selected.active) {
        QJsonArray edits;
        for (const auto &entry : selected.entries) {
            if (entry.index < 0 && entry.deleted) continue;
            if (entry.index >= 0 && !entry.deleted && sameFields(entry.object, entry.original)) continue;
            QJsonObject edit = objectJson(entry.object);
            edit.insert("array", entry.array);
            edit.insert("index", entry.index);
            edit.insert("deleted", entry.deleted);
            edits.append(edit);
        }
        QJsonObject difficulty{{"id", selected.descriptor.id}, {"file", selected.descriptor.filename}, {"edits", edits}};
        if (selected.generated) difficulty.insert("generated", true);
        difficulties.append(difficulty);
    }
    return {{"format", "LightsaberScoreProject"}, {"version", 1}, {"newSong", newSong},
            {"selectedDifficulty", current >= 0 ? tracks[current].descriptor.id : QString()},
            {"info", info}, {"firstBeatSeconds", newFirstBeatSeconds}, {"assetHashes", assetHashes},
            {"source", source}, {"difficulties", difficulties}};
}

bool BeatmapDocument::Impl::restoreState(const QJsonObject &state, QString *error) {
    if (state.value("format").toString() != "LightsaberScoreProject" || state.value("version").toInt() != 1)
        return fail(error, QStringLiteral("不是支持的编辑工程。"));
    if (state.value("assetHashes").toObject() != assetHashes)
        return fail(error, QStringLiteral("工程原始资源快照已改变或不完整，拒绝加载编辑记录。"));
    newSong = state.value("newSong").toBool();
    if (newSong) {
        const auto workingInfo = state.value("info").toObject();
        if (workingInfo.isEmpty()) return fail(error, QStringLiteral("工程缺少歌曲信息。"));
        const auto sets = workingInfo.value("_difficultyBeatmapSets").toArray();
        const auto maps = sets.first().toObject().value("_difficultyBeatmaps").toArray();
        const auto rows = state.value("difficulties").toArray();
        if (sets.size() != 1 || sets.first().toObject().value("_beatmapCharacteristicName").toString() != "Standard"
            || maps.isEmpty() || maps.size() > 128 || rows.size() != maps.size())
            return fail(error, QStringLiteral("新歌工程的 Standard 难度记录不完整。"));
        info = workingInfo;
        newFirstBeatSeconds = state.value("firstBeatSeconds").toDouble();
        if (!finite(newFirstBeatSeconds) || newFirstBeatSeconds < 0.0 || info.value("_songTimeOffset").toDouble() != 0.0)
            return fail(error, QStringLiteral("新歌对拍记录无效。"));
        QSet<QString> ids, files, names;
        for (const auto &rowValue : rows) {
            const auto row = rowValue.toObject();
            const QString id = row.value("id").toString(), file = row.value("file").toString();
            if (id.isEmpty() || ids.contains(id) || files.contains(file.toLower()))
                return fail(error, QStringLiteral("工程存在重复的难度 ID 或文件。"));
            ids.insert(id); files.insert(file.toLower());
            int found = -1;
            for (int i=0;i<tracks.size();++i) if (tracks[i].descriptor.id==id) { found=i; break; }
            if (found < 0) {
                const QString uuid = id.mid(QStringLiteral("generated:").size());
                if (!row.value("generated").toBool() || !id.startsWith("generated:") || QUuid(uuid).isNull()
                    || QUuid(uuid).toString(QUuid::WithoutBraces)!=uuid || file!="_generated-"+uuid+".dat")
                    return fail(error, QStringLiteral("新增难度恢复记录无效。"));
                for (auto it=assetHashes.begin();it!=assetHashes.end();++it)
                    if (it.key().compare(file,Qt::CaseInsensitive)==0)
                        return fail(error, QStringLiteral("新增难度不能覆盖原始资源。"));
                Track added; added.descriptor.id=id; added.descriptor.filename=file;
                added.descriptor.characteristic="Standard"; added.raw=emptyNewSongMap(); added.generated=true;
                if (!parseTrack(&added,error)) return false;
                tracks.append(std::move(added)); found=tracks.size()-1;
            }
            auto &selected=tracks[found];
            if (selected.descriptor.filename!=file || selected.generated!=row.value("generated").toBool())
                return fail(error, QStringLiteral("工程难度与原始快照不一致。"));
            QJsonObject descriptor;
            int matches=0;
            for (const auto &value : maps) if (value.toObject().value("_beatmapFilename").toString()==file) {
                descriptor=value.toObject(); ++matches;
            }
            const QString name=descriptor.value("_difficulty").toString();
            const int rank=descriptor.value("_difficultyRank").toInt();
            if (matches!=1 || name.trimmed().isEmpty() || names.contains(name.toLower()) || rank<1 || rank>99
                || !integral(descriptor.value("_difficultyRank")))
                return fail(error, QStringLiteral("新歌工程的难度名称、等级或文件映射无效。"));
            names.insert(name.toLower()); selected.descriptor.name=name; selected.descriptor.rank=rank;
            if (!selected.time.configure(info.value("_beatsPerMinute").toDouble(),newFirstBeatSeconds,{},error)) return false;
        }
        if (ids.size()!=tracks.size()) return fail(error, QStringLiteral("工程遗漏了原始难度，拒绝恢复。"));
    } else if (state.value("info").toObject() != info)
        return fail(error, QStringLiteral("已有谱工程的 Info 被意外修改，拒绝恢复。"));
    for (const auto &difficultyValue : state.value("difficulties").toArray()) {
        const auto difficulty = difficultyValue.toObject();
        const QString id = difficulty.value("id").toString();
        auto it = std::find_if(tracks.begin(), tracks.end(), [&id](const Track &selected) { return selected.descriptor.id == id; });
        if (it == tracks.end() || it->descriptor.filename != difficulty.value("file").toString())
            return fail(error, QStringLiteral("工程难度与原始快照不一致。"));
        const auto edits = difficulty.value("edits").toArray();
        QSet<QString> replaced;
        QVector<BeatObject> incoming;
        QSet<QString> seen;
        for (const auto &value : edits) {
            const auto edit = value.toObject();
            BeatObject object = objectFromJson(edit);
            if (seen.contains(object.id)) return fail(error, QStringLiteral("恢复数据有重复物件 ID。"));
            seen.insert(object.id);
            const int index = edit.value("index").toInt(-2);
            if (index >= 0) {
                const int e = it->byId.value(object.id, -1);
                if (e < 0 || it->entries[e].array != edit.value("array").toString() || it->entries[e].index != index ||
                    it->entries[e].object.isProtected() || it->entries[e].object.kind != object.kind)
                    return fail(error, QStringLiteral("恢复数据试图修改受保护或不存在的物件。"));
                replaced.insert(object.id);
            } else if (index != -1 || it->byId.contains(object.id) || object.id.isEmpty())
                return fail(error, QStringLiteral("新增物件的恢复 ID 无效。"));
            if (!edit.value("deleted").toBool()) incoming.append(object);
        }
        if (!edits.isEmpty() && !validate(*it, incoming, replaced, error)) return false;
        for (const auto &value : edits) {
            const auto edit = value.toObject();
            const BeatObject object = objectFromJson(edit);
            if (edit.value("index").toInt(-1) >= 0) {
                auto &entry = it->entries[it->byId.value(object.id)];
                entry.object = object;
                entry.deleted = edit.value("deleted").toBool();
            } else {
                const QString expected = object.kind == ObjectKind::Wall ? (it->v3 ? "obstacles" : "_obstacles") :
                        (it->v3 ? (object.kind == ObjectKind::Bomb ? "bombNotes" : "colorNotes") : "_notes");
                if (edit.value("array").toString() != expected) return fail(error, QStringLiteral("新增物件恢复数组不匹配。"));
                Entry entry;
                entry.object = object;
                entry.original = object;
                entry.array = expected;
                entry.deleted = edit.value("deleted").toBool();
                it->byId.insert(object.id, it->entries.size());
                it->entries.append(entry);
            }
        }
    }
    const QString selected = state.value("selectedDifficulty").toString();
    if (newSong && std::none_of(tracks.begin(),tracks.end(),[&selected](const Track &track) {
        return track.active && track.descriptor.id==selected;
    })) return fail(error,QStringLiteral("工程当前难度不存在。"));
    for (int i = 0; i < tracks.size(); ++i) if (tracks[i].descriptor.id == selected) current = i;
    refresh();
    return true;
}

BeatmapDocument::BeatmapDocument() : d(new Impl) {}
BeatmapDocument::~BeatmapDocument() = default;
BeatmapDocument::BeatmapDocument(BeatmapDocument &&) noexcept = default;
BeatmapDocument &BeatmapDocument::operator=(BeatmapDocument &&) noexcept = default;

bool BeatmapDocument::loadSong(const QString &folder, QString *error) {
    auto incoming = std::unique_ptr<Impl>(new Impl);
    incoming->temporary.reset(new QTemporaryDir(QDir::tempPath() + "/lmsc-song-XXXXXX"));
    if (!incoming->temporary->isValid()) return fail(error, QStringLiteral("无法建立导入快照目录。"));
    incoming->assets = QDir(incoming->temporary->path()).filePath("assets");
    if (!ProjectStore::copyTree(folder, incoming->assets, error) || !incoming->initialize(error)) return false;
    incoming->revision = d->revision + 1;
    d = std::move(incoming);
    return true;
}

bool BeatmapDocument::loadZip(const QString &filename, QString *error) {
    auto incoming = std::unique_ptr<Impl>(new Impl);
    incoming->temporary.reset(new QTemporaryDir(QDir::tempPath() + "/lmsc-zip-XXXXXX"));
    if (!incoming->temporary->isValid()) return fail(error, QStringLiteral("无法建立 ZIP 快照目录。"));
    incoming->assets = QDir(incoming->temporary->path()).filePath("assets");
    if (!ProjectStore::extractZip(filename, incoming->assets, error) || !incoming->initialize(error)) return false;
    incoming->revision = d->revision + 1;
    d = std::move(incoming);
    return true;
}

bool BeatmapDocument::loadProject(const QString &path, QString *error) {
    const QString file = normalizedManifest(path);
    QJsonObject json;
    if (!ProjectStore::readJson(file, &json, error)) return false;
    const QString assetsName = json.value("assets").toString();
    if (!ProjectStore::safeRelativePath(assetsName)) return fail(error, QStringLiteral("工程资源路径不安全。"));
    BeatmapDocument incoming;
    if (!incoming.loadSong(QDir(QFileInfo(file).absolutePath()).filePath(assetsName), error)) return false;
    if (!incoming.d->restoreState(json, error)) return false;
    const auto source = json.value("source").toObject();
    if (!source.isEmpty()) {
        const QString sourceAssets = json.value("sourceAssets").toString();
        const QString sourceFile = source.value("file").toString();
        if (!incoming.d->newSong || !ProjectStore::safeRelativePath(sourceAssets) || !ProjectStore::safeRelativePath(sourceFile))
            return fail(error, QStringLiteral("源媒体记录路径不安全。"));
        if (!integral(source.value("streamIndex")) || source.value("streamIndex").toInt() < 0 ||
            !source.value("startSeconds").isDouble() || !source.value("endSeconds").isDouble() ||
            source.value("startSeconds").toDouble() < 0 || source.value("endSeconds").toDouble() <= source.value("startSeconds").toDouble())
            return fail(error, QStringLiteral("工程音轨或裁剪记录无效。"));
        const QString sourceRoot = QDir(QFileInfo(file).absolutePath()).filePath(sourceAssets);
        incoming.d->sourceRoot = QDir(incoming.d->temporary->path()).filePath("source");
        if (!ProjectStore::copyTree(sourceRoot, incoming.d->sourceRoot, error)) return false;
        QFile media(QDir(incoming.d->sourceRoot).filePath(sourceFile));
        if (!media.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("工程源媒体丢失。"));
        QCryptographicHash digest(QCryptographicHash::Sha256);
        if (!digest.addData(&media) || QString::fromLatin1(digest.result().toHex()) != source.value("sha256").toString())
            return fail(error, QStringLiteral("工程源媒体摘要不一致。"));
        incoming.d->source = source;
    }
    const bool recovery = QFileInfo(file).fileName().compare("autosave.lmsc", Qt::CaseInsensitive) == 0;
    incoming.d->manifest = recovery ? QDir(QFileInfo(file).absolutePath()).filePath(json.value("savedManifest").toString("project.lmsc")) : file;
    if (recovery && !ProjectStore::safeRelativePath(json.value("savedManifest").toString("project.lmsc")))
        return fail(error, QStringLiteral("恢复工程路径不安全。"));
    if (recovery) {
        QJsonObject checkpoint;
        if (!ProjectStore::readJson(incoming.d->manifest, &checkpoint, error) ||
            checkpoint.value("checkpoint") != json.value("checkpoint"))
            return fail(error, QStringLiteral("此自动快照已被较新的手动保存取代。"));
    }
    if (recovery) incoming.d->metaKey = incoming.d->nextKey++;
    incoming.d->revision = d->revision + 1;
    d = std::move(incoming.d);
    return true;
}

bool BeatmapDocument::createNew(const QString &audioPath, const QString &title, double bpm,
                               double firstBeatSeconds, const QString &coverPath, QString *error) {
    TimeMap timing;
    if (!timing.configure(bpm, firstBeatSeconds, {}, error)) return false;
    if (firstBeatSeconds < 0.0) return fail(error, QStringLiteral("第一拍不能早于裁剪后音频的起点。"));
    if (title.trimmed().isEmpty()) return fail(error, QStringLiteral("请输入歌名。"));
    if (!QFileInfo(audioPath).isFile() || QFileInfo(audioPath).suffix().compare("ogg", Qt::CaseInsensitive) != 0)
        return fail(error, QStringLiteral("新歌需先转换为 Ogg 音频。"));
    auto incoming = std::unique_ptr<Impl>(new Impl);
    incoming->temporary.reset(new QTemporaryDir(QDir::tempPath() + "/lmsc-new-XXXXXX"));
    if (!incoming->temporary->isValid()) return fail(error, QStringLiteral("无法建立新歌工程。"));
    incoming->assets = QDir(incoming->temporary->path()).filePath("assets");
    if (!QDir().mkpath(incoming->assets) || !QFile::copy(audioPath, QDir(incoming->assets).filePath("song.ogg")))
        return fail(error, QStringLiteral("无法复制已转换音频到工程。"));
    QString coverName;
    if (!coverPath.isEmpty()) {
        const QString suffix = QFileInfo(coverPath).suffix().toLower();
        if (!QStringList{"png", "jpg", "jpeg"}.contains(suffix) || !QFileInfo(coverPath).isFile())
            return fail(error, QStringLiteral("封面必须是 PNG/JPEG 文件。"));
        coverName = "cover." + suffix;
        if (!QFile::copy(coverPath, QDir(incoming->assets).filePath(coverName))) return fail(error, QStringLiteral("无法复制封面。"));
    }
    const QJsonObject difficulty{{"_difficulty", "Expert"}, {"_difficultyRank", 7}, {"_beatmapFilename", "Expert.dat"},
                                 {"_noteJumpMovementSpeed", 12}, {"_noteJumpStartBeatOffset", 0}};
    incoming->info = {{"_version", "2.0.0"}, {"_songName", title.trimmed()}, {"_songSubName", ""},
                      {"_songAuthorName", ""}, {"_levelAuthorName", ""}, {"_beatsPerMinute", bpm},
                      {"_songTimeOffset", 0}, {"_shuffle", 0}, {"_shufflePeriod", 0.5},
                      {"_previewStartTime", 0}, {"_previewDuration", 10}, {"_songFilename", "song.ogg"},
                      {"_coverImageFilename", coverName}, {"_environmentName", "DefaultEnvironment"},
                      {"_allDirectionsEnvironmentName", "GlassDesertEnvironment"}, {"_songApproximativeDuration", 0},
                      {"_difficultyBeatmapSets", QJsonArray{QJsonObject{{"_beatmapCharacteristicName", "Standard"},
                                                                    {"_difficultyBeatmaps", QJsonArray{difficulty}}}}}};
    // Dry Hands (the user-confirmed playable baseline) uses this basic format.
    QJsonObject beatmap{{"_version", "2.2.0"}, {"_notes", QJsonArray{}},
                        {"_obstacles", QJsonArray{}}, {"_events", QJsonArray{}}, {"_waypoints", QJsonArray{}}};
    if (!ProjectStore::writeJson(QDir(incoming->assets).filePath("Info.dat"), incoming->info, error) ||
        !ProjectStore::writeJson(QDir(incoming->assets).filePath("Expert.dat"), beatmap, error) || !incoming->initialize(error)) return false;
    incoming->newSong = true;
    incoming->newFirstBeatSeconds = firstBeatSeconds;
    incoming->tracks[0].time = timing;
    incoming->metaKey = incoming->nextKey++;
    incoming->revision = d->revision + 1;
    d = std::move(incoming);
    return true;
}

bool BeatmapDocument::isLoaded() const { return d && !d->tracks.isEmpty(); }
QString BeatmapDocument::title() const { return d->info.value("_songName").toString(); }
QString BeatmapDocument::audioPath() const { return d->audioRelative.isEmpty() ? QString() : d->absolute(d->audioRelative); }
QString BeatmapDocument::coverPath() const { return d->coverRelative.isEmpty() ? QString() : d->absolute(d->coverRelative); }
QString BeatmapDocument::projectPath() const { return d->manifest; }
bool BeatmapDocument::isNewSong() const { return d->newSong; }
const QVector<Difficulty> &BeatmapDocument::difficulties() const { return d->descriptors; }
QString BeatmapDocument::currentDifficultyId() const { return d->track() ? d->track()->descriptor.id : QString(); }
const QVector<BeatObject> &BeatmapDocument::objects() const { return d->view; }
int BeatmapDocument::objectCount(const QString &difficultyId) const {
    for (const auto &selected : d->tracks) if (selected.active && selected.descriptor.id==difficultyId) {
        int count=0;
        for (const auto &entry : selected.entries) if (!entry.deleted) ++count;
        return count;
    }
    return 0;
}
const TimeMap &BeatmapDocument::timeMap() const { static const TimeMap fallback; return d->track() ? d->track()->time : fallback; }
QString BeatmapDocument::readOnlyReason() const { return d->track() ? d->track()->readOnly : QStringLiteral("尚未打开曲谱。" ); }
QStringList BeatmapDocument::warnings() const { return d->warnings; }
quint64 BeatmapDocument::revision() const { return d->revision; }

bool BeatmapDocument::setDifficulty(const QString &id, QString *error) {
    for (int i = 0; i < d->tracks.size(); ++i) if (d->tracks[i].active && d->tracks[i].descriptor.id == id) {
        if (d->current != i) { d->current = i; ++d->revision; }
        d->refresh(); return true;
    }
    return fail(error, QStringLiteral("未找到指定难度。"));
}

bool BeatmapDocument::addObject(const BeatObject &object, QString *error) {
    return pasteObjects({object}, 0.0, 0, false, error);
}

bool BeatmapDocument::updateObject(const BeatObject &object, QString *error) { return updateObjects({object}, error); }

bool BeatmapDocument::updateObjects(const QVector<BeatObject> &objects, QString *error) {
    auto *selected = d->track();
    if (!selected) return fail(error, QStringLiteral("请先打开曲谱。"));
    QSet<QString> ids;
    QVector<Impl::Change> changes;
    for (const auto &object : objects) {
        const int index = selected->byId.value(object.id, -1);
        if (index < 0 || selected->entries[index].deleted || ids.contains(object.id))
            return fail(error, QStringLiteral("批量选择含重复或不存在的物件，未执行。"));
        const auto &entry = selected->entries[index];
        if (entry.object.isProtected()) return fail(error, entry.object.protectedReason);
        if (entry.object.kind != object.kind) return fail(error, QStringLiteral("不能通过属性更改物件类型。"));
        ids.insert(object.id);
        if (!sameFields(object, entry.object)) changes.append({index, {entry.object, false}, {object, false}});
    }
    if (!d->validate(*selected, objects, ids, error)) return false;
    return d->commit(selected, std::move(changes), error);
}

bool BeatmapDocument::removeObjects(const QStringList &ids, QString *error) {
    auto *selected = d->track();
    if (!selected) return fail(error, QStringLiteral("请先打开曲谱。"));
    if (!selected->readOnly.isEmpty()) return fail(error, selected->readOnly);
    QSet<QString> seen;
    QVector<Impl::Change> changes;
    for (const auto &id : ids) {
        const int index = selected->byId.value(id, -1);
        if (index < 0 || selected->entries[index].deleted || seen.contains(id))
            return fail(error, QStringLiteral("删除选择含不存在或重复的物件。"));
        const auto &entry = selected->entries[index];
        if (entry.object.isProtected()) return fail(error, entry.object.protectedReason);
        seen.insert(id);
        changes.append({index, {entry.object, false}, {entry.object, true}});
    }
    return d->commit(selected, std::move(changes), error);
}

QVector<BeatObject> BeatmapDocument::copyObjects(const QStringList &ids, QString *error) const {
    const auto *selected = d->track();
    QVector<BeatObject> output;
    if (!selected) { fail(error, QStringLiteral("请先打开曲谱。")); return {}; }
    if (!selected->readOnly.isEmpty()) { fail(error, selected->readOnly); return {}; }
    QSet<QString> seen;
    for (const auto &id : ids) {
        const int index = selected->byId.value(id, -1);
        if (index < 0 || selected->entries[index].deleted || seen.contains(id)) {
            fail(error, QStringLiteral("复制选择无效。")); return {};
        }
        const auto &object = selected->entries[index].object;
        if (object.isProtected()) { fail(error, object.protectedReason); return {}; }
        seen.insert(id);
        output.append(object);
    }
    return output;
}

bool BeatmapDocument::pasteObjects(const QVector<BeatObject> &objects, double beatOffset,
                                  int xOffset, bool mirror, QString *error) {
    auto *selected = d->track();
    if (!selected) return fail(error, QStringLiteral("请先打开曲谱。"));
    QVector<BeatObject> incoming = objects;
    for (auto &object : incoming) {
        if (object.isProtected()) return fail(error, object.protectedReason);
        // A caller cannot clear a protected source flag and paste its original ID.
        for (const auto &difficulty : d->tracks) {
            const int source = difficulty.byId.value(object.id, -1);
            if (source >= 0 && difficulty.entries[source].object.isProtected()) return fail(error, difficulty.entries[source].object.protectedReason);
        }
        object.id = newId();
        object.beat += beatOffset;
        if (mirror) {
            object.x = object.kind == ObjectKind::Wall ? 4 - object.x - object.width : 3 - object.x;
            if (object.kind == ObjectKind::Note) { object.color = 1 - object.color; object.direction = mirroredDirection(object.direction); }
        }
        object.x += xOffset;
    }
    if (!d->validate(*selected, incoming, {}, error)) return false;
    if (qint64(incoming.size()) * sizeof(Impl::Change) > 16 * 1024 * 1024)
        return fail(error, QStringLiteral("本次粘贴超过批量操作上限，请分段执行。"));
    QVector<Impl::Change> changes;
    for (const auto &object : incoming) {
        Impl::Entry entry;
        entry.object = object;
        entry.original = object;
        entry.deleted = true;
        entry.array = object.kind == ObjectKind::Wall ? (selected->v3 ? "obstacles" : "_obstacles") :
                (selected->v3 ? (object.kind == ObjectKind::Bomb ? "bombNotes" : "colorNotes") : "_notes");
        const int index = selected->entries.size();
        selected->byId.insert(object.id, index);
        selected->entries.append(entry);
        changes.append({index, {object, true}, {object, false}});
    }
    return d->commit(selected, std::move(changes), error);
}

bool BeatmapDocument::mirrorObjects(const QStringList &ids, QString *error) {
    auto objects = copyObjects(ids, error);
    if (objects.size() != ids.size()) return false;
    for (auto &object : objects) {
        object.x = object.kind == ObjectKind::Wall ? 4 - object.x - object.width : 3 - object.x;
        if (object.kind == ObjectKind::Note) { object.color = 1 - object.color; object.direction = mirroredDirection(object.direction); }
    }
    return updateObjects(objects, error);
}

bool BeatmapDocument::applyGeneratedChart(const QVector<BeatObject> &objects,
                                           const QString &difficultyName, int difficultyRank,
                                           quint64 expectedRevision, QString *error) {
    auto *selected = d->track();
    if (!d->newSong || !selected || selected->descriptor.characteristic != "Standard")
        return fail(error, QStringLiteral("整曲自动制谱仅应用到新建歌曲的 Standard 谱。"));
    if (selected->v3 || selected->descriptor.version != "2.2.0" || !selected->time.changes().isEmpty())
        return fail(error, QStringLiteral("自动制谱首版使用基础 v2.2 新歌谱面，保留已有格式与变速内容。"));
    if (expectedRevision != d->revision)
        return fail(error, QStringLiteral("歌曲或谱面已更改，请重新生成候选谱。"));
    const QMap<QString, int> ranks{{"Easy", 1}, {"Normal", 3}, {"Hard", 5}, {"Expert", 7}, {"ExpertPlus", 9}};
    if (!ranks.contains(difficultyName) || ranks.value(difficultyName) != difficultyRank)
        return fail(error, QStringLiteral("自动制谱的难度名称与等级不匹配。"));
    const int beforeCurrent = d->current;
    int target = -1;
    for (int i=0;i<d->tracks.size();++i)
        if (d->tracks[i].active && d->tracks[i].descriptor.name.compare(difficultyName,Qt::CaseInsensitive)==0) { target=i; break; }
    // Reuse only the initial empty placeholder. A populated chart always keeps
    // its identity, even when generation was started from that difficulty.
    if (target<0 && d->descriptors.size()==1 && d->view.isEmpty() && selected->entries.isEmpty()) target=d->current;
    const bool added = target<0;
    Impl::Track staged;
    if (added) {
        const QString uuid=newId();
        staged.descriptor={"generated:"+uuid,"Standard",difficultyName,"_generated-"+uuid+".dat",difficultyRank,"2.2.0"};
        staged.raw=emptyNewSongMap(); staged.time=selected->time;
        staged.generated=true; staged.savedActive=false;
        selected=&staged;
    } else selected=&d->tracks[target];
    if (selected->v3 || selected->descriptor.version!="2.2.0" || !selected->time.changes().isEmpty())
        return fail(error, QStringLiteral("目标难度包含非基础格式或变速内容，未应用。"));
    if (!selected->readOnly.isEmpty()) return fail(error, selected->readOnly);

    QSet<QString> replaced;
    QVector<Impl::Change> changes;
    for (int i = 0; i < selected->entries.size(); ++i) {
        const auto &entry = selected->entries[i];
        if (entry.deleted) continue;
        if (entry.object.isProtected()) return fail(error, entry.object.protectedReason);
        replaced.insert(entry.object.id);
        changes.append({i, {entry.object, false}, {entry.object, true}});
    }
    if ((qint64(changes.size()) + objects.size()) * sizeof(Impl::Change) > 16 * 1024 * 1024)
        return fail(error, QStringLiteral("生成结果超过整批撤销上限，未应用。"));
    QVector<BeatObject> incoming = objects;
    for (auto &object : incoming) {
        if (object.isProtected()) return fail(error, object.protectedReason);
        for (const auto &track : d->tracks) {
            const int source = track.byId.value(object.id, -1);
            if (source >= 0 && track.entries[source].object.isProtected())
                return fail(error, track.entries[source].object.protectedReason);
        }
        object.id = newId();
        if (object.kind == ObjectKind::Bomb) { object.color = 0; object.direction = 8; }
    }
    if (!d->validate(*selected, incoming, replaced, error)) return false;

    // No fallible validation follows allocation of entries or difficulty changes.
    for (const auto &object : incoming) {
        Impl::Entry entry;
        entry.object = entry.original = object;
        entry.deleted = true;
        entry.array = object.kind == ObjectKind::Wall ? "_obstacles" : "_notes";
        const int index = selected->entries.size();
        selected->byId.insert(object.id, index);
        selected->entries.append(entry);
        changes.append({index, {object, true}, {object, false}});
    }
    if (added) { d->tracks.append(std::move(staged)); target=d->tracks.size()-1; selected=&d->tracks[target]; }
    d->current=target;
    const bool applied=d->commit(selected,std::move(changes),error,difficultyName,difficultyRank,added,beforeCurrent);
    d->refresh();
    return applied;
}

bool BeatmapDocument::canUndo() const { return d->newSong ? d->songCursor>0 : d->track() && d->track()->cursor > 0; }
bool BeatmapDocument::canRedo() const { return d->newSong ? d->songCursor<d->songHistory.size() : d->track() && d->track()->cursor < d->track()->history.size(); }
bool BeatmapDocument::undo() {
    if (!canUndo()) return false;
    if (d->newSong) {
        const auto &command=d->songHistory[--d->songCursor];
        auto *selected=&d->tracks[command.track];
        for (const auto &change : command.edit.changes) {
            selected->entries[change.entry].object=change.before.object;
            selected->entries[change.entry].deleted=change.before.deleted;
        }
        selected->stateKey=command.edit.beforeKey;
        if (command.edit.difficultyChanged) d->applyDifficulty(selected,command.edit.beforeDifficulty);
        if (command.added) selected->active=false;
        d->current=command.added ? command.beforeCurrent : command.track;
        d->syncNewSongDifficulties(); ++d->revision; d->refresh(); return true;
    }
    auto *selected = d->track();
    const auto &command = selected->history[--selected->cursor];
    for (const auto &change : command.changes) {
        selected->entries[change.entry].object = change.before.object;
        selected->entries[change.entry].deleted = change.before.deleted;
    }
    selected->stateKey = command.beforeKey;
    if (command.difficultyChanged) d->applyDifficulty(selected, command.beforeDifficulty);
    ++d->revision;
    d->refresh();
    return true;
}
bool BeatmapDocument::redo() {
    if (!canRedo()) return false;
    if (d->newSong) {
        const auto &command=d->songHistory[d->songCursor++];
        auto *selected=&d->tracks[command.track];
        selected->active=true;
        for (const auto &change : command.edit.changes) {
            selected->entries[change.entry].object=change.after.object;
            selected->entries[change.entry].deleted=change.after.deleted;
        }
        selected->stateKey=command.edit.afterKey;
        if (command.edit.difficultyChanged) d->applyDifficulty(selected,command.edit.afterDifficulty);
        d->current=command.track;
        d->syncNewSongDifficulties(); ++d->revision; d->refresh(); return true;
    }
    auto *selected = d->track();
    const auto &command = selected->history[selected->cursor++];
    for (const auto &change : command.changes) {
        selected->entries[change.entry].object = change.after.object;
        selected->entries[change.entry].deleted = change.after.deleted;
    }
    selected->stateKey = command.afterKey;
    if (command.difficultyChanged) d->applyDifficulty(selected, command.afterDifficulty);
    ++d->revision;
    d->refresh();
    return true;
}
bool BeatmapDocument::isModified() const {
    if (d->metaKey != d->savedMetaKey) return true;
    for (const auto &selected : d->tracks)
        if (selected.active!=selected.savedActive || (selected.active && selected.stateKey!=selected.savedKey)) return true;
    return false;
}

bool BeatmapDocument::setNewSongTempo(double bpm, double firstBeatSeconds, QString *error) {
    if (!d->newSong || !d->track()) return fail(error, QStringLiteral("已有谱的 BPM 与偏移保持原样。"));
    TimeMap time;
    if (!time.configure(bpm, firstBeatSeconds, {}, error)) return false;
    if (firstBeatSeconds < 0.0) return fail(error, QStringLiteral("第一拍不能早于裁剪后音频的起点。"));
    d->info.insert("_beatsPerMinute", bpm);
    d->info.insert("_songTimeOffset", 0);
    d->newFirstBeatSeconds = firstBeatSeconds;
    for (auto &selected : d->tracks) selected.time = time;
    d->metaKey = d->nextKey++;
    ++d->revision;
    return true;
}

bool BeatmapDocument::setNewSongDifficulty(const QString &name, int rank, QString *error) {
    if (!d->newSong || d->tracks.isEmpty()) return fail(error, QStringLiteral("已有谱的难度标识保持原样。"));
    if (name.trimmed().isEmpty() || rank <= 0 || rank > 99) return fail(error, QStringLiteral("难度名称或等级标识无效。"));
    for (const auto &selected : d->tracks)
        if (selected.active && selected.descriptor.name.compare(name.trimmed(),Qt::CaseInsensitive)==0
            && selected.descriptor.id!=currentDifficultyId()) return setDifficulty(selected.descriptor.id,error);
    return d->commit(d->track(), {}, error, name.trimmed(), rank);
}

bool BeatmapDocument::setNewSongMetadata(const QString &title, const QString &artist,
                                       const QString &mapper, QString *error) {
    if (!d->newSong) return fail(error, QStringLiteral("已有谱的歌曲元数据保持原样。"));
    if (title.trimmed().isEmpty()) return fail(error, QStringLiteral("歌名不能为空。"));
    d->info.insert("_songName", title.trimmed());
    d->info.insert("_songAuthorName", artist.trimmed());
    d->info.insert("_levelAuthorName", mapper.trimmed());
    d->metaKey = d->nextKey++;
    ++d->revision;
    return true;
}

bool BeatmapDocument::setImportSource(const QString &path, int absoluteStreamIndex,
                                    double startSeconds, double endSeconds, QString *error) {
    if (!d->newSong || !isLoaded()) return fail(error, QStringLiteral("仅新歌保存导入源媒体与裁剪记录。"));
    const QFileInfo input(path);
    if (!input.isFile() || input.isSymLink() || absoluteStreamIndex < 0 || !finite(startSeconds) || startSeconds < 0 ||
        !finite(endSeconds) || endSeconds <= startSeconds)
        return fail(error, QStringLiteral("源媒体、音轨或裁剪范围无效。"));
    const QString root = QDir(d->temporary->path()).filePath("source-" + newId());
    if (!QDir().mkpath(root)) return fail(error, QStringLiteral("无法建立源媒体快照。"));
    const QString extension = input.suffix().toLower();
    const QString name = "original" + (extension.isEmpty() ? QString() : "." + extension);
    if (!ProjectStore::safeRelativePath(name) || !QFile::copy(path, QDir(root).filePath(name)))
        return fail(error, QStringLiteral("无法复制原媒体到工程。"));
    QFile copied(QDir(root).filePath(name));
    if (!copied.open(QIODevice::ReadOnly)) return fail(error, copied.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&copied)) return fail(error, QStringLiteral("无法计算源媒体摘要。"));
    d->sourceRoot = root;
    d->source = {{"file", name}, {"originalFilename", input.fileName()}, {"streamIndex", absoluteStreamIndex},
                 {"startSeconds", startSeconds}, {"endSeconds", endSeconds}, {"sha256", QString::fromLatin1(hash.result().toHex())}};
    d->metaKey = d->nextKey++;
    ++d->revision;
    return true;
}

ImportSource BeatmapDocument::importSource() const {
    ImportSource result;
    if (!d->source.isEmpty()) {
        result.path = QDir(d->sourceRoot).filePath(d->source.value("file").toString());
        result.streamIndex = d->source.value("streamIndex").toInt(-1);
        result.startSeconds = d->source.value("startSeconds").toDouble();
        result.endSeconds = d->source.value("endSeconds").toDouble(-1);
    }
    return result;
}

bool BeatmapDocument::saveProject(const QString &path, QString *error) {
    if (!isLoaded()) return fail(error, QStringLiteral("请先打开或建立工程。"));
    const QString file = normalizedManifest(path);
    if (QFileInfo(file).suffix().compare("lmsc", Qt::CaseInsensitive) != 0)
        return fail(error, QStringLiteral("工程清单使用 .lmsc 后缀。"));
    if (QFileInfo(file).fileName().compare("autosave.lmsc", Qt::CaseInsensitive) == 0)
        return fail(error, QStringLiteral("autosave.lmsc 保留给恢复快照，请选择其他工程名。"));
    const QString parent = QFileInfo(file).absolutePath();
    if (!QDir().mkpath(parent)) return fail(error, QStringLiteral("无法建立工程目录。"));
    QString assetName;
    QJsonObject previous;
    if (d->manifest == file && QFileInfo::exists(file)) {
        if (!ProjectStore::readJson(file, &previous, error)) return false;
        assetName = previous.value("assets").toString();
        if (!ProjectStore::safeRelativePath(assetName) || !QFileInfo(QDir(parent).filePath(assetName)).isDir())
            return fail(error, QStringLiteral("已保存工程的资源快照丢失。"));
    } else {
        if (QFileInfo::exists(file)) return fail(error, QStringLiteral("目标已有另一份工程，请选择新位置。"));
        assetName = "assets-" + newId();
        if (!ProjectStore::copyTree(d->assets, QDir(parent).filePath(assetName), error)) return false;
    }
    QJsonObject state = d->state();
    state.insert("assets", assetName);
    state.insert("checkpoint", newId());
    if (!d->source.isEmpty()) {
        QString sourceAssets;
        if (previous.value("source").toObject() == d->source &&
            ProjectStore::safeRelativePath(previous.value("sourceAssets").toString()))
            sourceAssets = previous.value("sourceAssets").toString();
        else {
            sourceAssets = "source-" + newId();
            if (!ProjectStore::copyTree(d->sourceRoot, QDir(parent).filePath(sourceAssets), error)) return false;
        }
        state.insert("sourceAssets", sourceAssets);
    }
    if (!ProjectStore::writeJson(file, state, error)) return false;
    d->manifest = file;
    for (auto &selected : d->tracks) { selected.savedKey = selected.stateKey; selected.savedActive=selected.active; }
    d->savedMetaKey = d->metaKey;
    return true;
}

QString BeatmapDocument::recoveryPath(const QString &projectPath) {
    const QString manifest = normalizedManifest(projectPath);
    return QDir(QFileInfo(manifest).absolutePath()).filePath("autosave.lmsc");
}

bool BeatmapDocument::autoSave(QString *error) {
    if (d->manifest.isEmpty()) return fail(error, QStringLiteral("请先保存工程一次，再启用自动恢复。"));
    QJsonObject saved;
    if (!ProjectStore::readJson(d->manifest, &saved, error)) return false;
    QJsonObject state = d->state();
    state.insert("assets", saved.value("assets"));
    state.insert("checkpoint", saved.value("checkpoint"));
    state.insert("savedManifest", QFileInfo(d->manifest).fileName());
    if (!d->source.isEmpty()) {
        // Changing an import source requires a normal save to snapshot its media first.
        if (saved.value("source").toObject() != d->source)
            return fail(error, QStringLiteral("源媒体记录已改变，请先手动保存工程。"));
        state.insert("sourceAssets", saved.value("sourceAssets"));
    }
    return ProjectStore::writeJson(recoveryPath(d->manifest), state, error);
}

bool BeatmapDocument::hasRecovery(const QString &projectPath) {
    const QString savedFile = normalizedManifest(projectPath);
    QJsonObject saved, recovery;
    if (!ProjectStore::readJson(savedFile, &saved) || !ProjectStore::readJson(recoveryPath(savedFile), &recovery)) return false;
    if (recovery.value("savedManifest").toString() != QFileInfo(savedFile).fileName()) return false;
    recovery.remove("savedManifest");
    // Original asset snapshots must belong to this exact project.
    return recovery.value("assets") == saved.value("assets") && recovery.value("checkpoint") == saved.value("checkpoint") && recovery != saved;
}

bool BeatmapDocument::exportSong(const QString &destinationFolder, QString *error) const {
    if (!isLoaded()) return fail(error, QStringLiteral("请先打开或建立工程。"));
    const QFileInfo target(destinationFolder);
    if (target.exists()) return fail(error, QStringLiteral("导出目录必须是新目录，未覆盖已有歌曲。"));
    const QString parent = target.absolutePath();
    if (!QDir().mkpath(parent)) return fail(error, QStringLiteral("无法建立导出父目录。"));
    QTemporaryDir staging(QDir(parent).filePath(".lmsc-export-XXXXXX"));
    if (!staging.isValid()) return fail(error, QStringLiteral("无法建立导出暂存目录。"));
    const QString song = QDir(staging.path()).filePath("song");
    if (!ProjectStore::copyTree(d->assets, song, error)) return false;
    for (auto it = d->assetHashes.begin(); it != d->assetHashes.end(); ++it) {
        QFile original(QDir(song).filePath(it.key()));
        if (!original.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("导出资源丢失。"));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&original) || QString::fromLatin1(hash.result().toHex()) != it.value().toString())
            return fail(error, QStringLiteral("原始快照发生变化，拒绝导出：%1").arg(it.key()));
    }
    for (const auto &selected : d->tracks) if (selected.active && d->changed(selected)) {
        if (!selected.readOnly.isEmpty()) return fail(error, QStringLiteral("受保护难度出现编辑记录，拒绝导出。"));
        const auto merged = d->merged(selected);
        if (!ProjectStore::writeJson(QDir(song).filePath(selected.descriptor.filename), merged, error)) return false;
        // All untouched top-level arrays/keys must remain semantically identical.
        const QStringList editable = selected.v3 ? QStringList{"colorNotes", "bombNotes", "obstacles"} : QStringList{"_notes", "_obstacles"};
        for (auto it = selected.raw.begin(); it != selected.raw.end(); ++it)
            if (!editable.contains(it.key()) && merged.value(it.key()) != it.value())
                return fail(error, QStringLiteral("导出校验发现高级或未知字段改变。"));
    }
    if (d->newSong) {
        // Materialize every map before writing targets: the original Expert.dat
        // template may belong to Easy while a later track exports as Expert.dat.
        const QStringList standardNames{"Easy", "Normal", "Hard", "Expert", "ExpertPlus"};
        QMap<QString,QJsonObject> maps;
        QMap<QString,QString> filenames;
        QSet<QString> targets;
        QSet<QString> originalMaps;
        for (const auto &selected : d->tracks) if (!selected.generated) originalMaps.insert(selected.descriptor.filename.toLower());
        for (const auto &selected : d->tracks) if (selected.active) {
            const QString file=standardNames.contains(selected.descriptor.name)
                ? selected.descriptor.name+".dat" : selected.descriptor.filename;
            if (!ProjectStore::safeRelativePath(file) || targets.contains(file.toLower())
                || file.compare(d->infoRelative,Qt::CaseInsensitive)==0
                || file.compare(d->audioRelative,Qt::CaseInsensitive)==0
                || file.compare(d->coverRelative,Qt::CaseInsensitive)==0)
                return fail(error,QStringLiteral("导出难度文件发生冲突，未完成导出。"));
            for (auto asset=d->assetHashes.begin();asset!=d->assetHashes.end();++asset)
                if (asset.key().compare(file,Qt::CaseInsensitive)==0 && !originalMaps.contains(asset.key().toLower()))
                    return fail(error,QStringLiteral("导出难度文件与其他原始资源冲突，未完成导出。"));
            targets.insert(file.toLower()); maps.insert(file,d->merged(selected));
            filenames.insert(selected.descriptor.filename,file);
        }
        auto exportedInfo = d->info;
        auto sets = exportedInfo.value("_difficultyBeatmapSets").toArray();
        auto set = sets.first().toObject();
        auto difficulties = set.value("_difficultyBeatmaps").toArray();
        for (int i=0;i<difficulties.size();++i) {
            auto difficulty=difficulties[i].toObject();
            difficulty.insert("_beatmapFilename",filenames.value(difficulty.value("_beatmapFilename").toString()));
            difficulties[i]=difficulty;
        }
        set.insert("_difficultyBeatmaps", difficulties);
        sets[0] = set;
        exportedInfo.insert("_difficultyBeatmapSets", sets);
        for (const auto &selected : d->tracks) if (selected.active) {
            const QString path=QDir(song).filePath(selected.descriptor.filename);
            if (QFileInfo::exists(path) && !QFile::remove(path))
                return fail(error,QStringLiteral("无法整理导出暂存谱面。"));
        }
        for (auto it=maps.begin();it!=maps.end();++it)
            if (!ProjectStore::writeJson(QDir(song).filePath(it.key()),it.value(),error)) return false;
        if (!ProjectStore::writeJson(QDir(song).filePath(d->infoRelative), exportedInfo, error)) return false;
    }
    if (!QDir().rename(song, target.absoluteFilePath()))
        return fail(error, QStringLiteral("无法完成导出目录提交；目标可能已被创建。"));
    return true;
}

} // namespace lmsc
