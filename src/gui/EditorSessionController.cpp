#include "EditorSessionController.h"
#include "core/MusicFeatureAnalyzer.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <functional>

namespace lmsc {
namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
QJsonObject timeJson(const TimeMap &map) {
    QJsonArray changes;
    for (const auto &change : map.changes()) changes.append(QJsonObject{{"beat", change.beat}, {"bpm", change.bpm}});
    return {{"bpm", map.initialBpm()}, {"offsetSeconds", map.offsetSeconds()}, {"changes", changes}};
}
QString fileHash(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { fail(error, QStringLiteral("草稿关联的音频或源媒体不可读取。")); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) { fail(error, QStringLiteral("无法核对草稿关联的媒体指纹。")); return {}; }
    return QString::fromLatin1(hash.result().toHex());
}
QJsonObject resourceIdentity(const BeatmapDocument &document, QString *error,
                            const std::function<QString(const QString &, QString *)> &hasher) {
    const auto audioHash = hasher(document.audioPath(), error);
    if (audioHash.isEmpty()) return {};
    QJsonObject source{{"available", document.importSource().isAvailable()}};
    const auto import = document.importSource();
    if (import.isAvailable()) {
        const auto sourceHash = hasher(import.path, error);
        if (sourceHash.isEmpty()) return {};
        source.insert("sha256", sourceHash); source.insert("streamIndex", import.streamIndex);
        source.insert("startSeconds", import.startSeconds); source.insert("endSeconds", import.endSeconds);
    }
    return {{"audioReference", "formalAudio"}, {"audioSha256", audioHash},
        {"time", timeJson(document.timeMap())}, {"source", source}, {"newSong", document.isNewSong()}};
}
const Difficulty *findTarget(const BeatmapDocument &document, const QString &name) {
    for (const auto &difficulty : document.difficulties())
        if (difficulty.characteristic == "Standard" && difficulty.name == name) return &difficulty;
    return nullptr;
}
bool number(const QJsonObject &json, const char *key, double *value) {
    const auto item = json.value(QLatin1String(key));
    if (!item.isDouble() || !std::isfinite(item.toDouble())) return false;
    *value = item.toDouble(); return true;
}
bool integer(const QJsonObject &json, const char *key, int *value, int minimum, int maximum) {
    double item = 0;
    if (!number(json, key, &item) || item < minimum || item > maximum || item != std::floor(item)) return false;
    *value = int(item); return true;
}
QJsonArray objectsJson(const QVector<BeatObject> &objects) {
    QJsonArray rows;
    for (const auto &object : objects) rows.append(QJsonObject{{"id", object.id}, {"kind", int(object.kind)},
        {"beat", object.beat}, {"x", object.x}, {"y", object.y}, {"color", object.color},
        {"direction", object.direction}, {"duration", object.duration}, {"width", object.width},
        {"height", object.height}, {"customData", object.preservedCustomData}, {"protectedReason", object.protectedReason}});
    return rows;
}
bool readObjects(const QJsonValue &value, QVector<BeatObject> *objects) {
    if (!value.isArray() || value.toArray().size() > 1000000) return false;
    QVector<BeatObject> parsed; QSet<QString> ids;
    for (const auto &row : value.toArray()) {
        if (!row.isObject()) return false;
        const auto json = row.toObject(); BeatObject object; int kind = 0;
        if (!json.value("id").isString() || json.value("id").toString().isEmpty()
                || !integer(json, "kind", &kind, 0, 2) || !number(json, "beat", &object.beat)
                || !integer(json, "x", &object.x, -100000, 100000)
                || !integer(json, "y", &object.y, -100000, 100000)
                || !integer(json, "color", &object.color, 0, 1)
                || !integer(json, "direction", &object.direction, 0, 8)
                || !number(json, "duration", &object.duration)
                || !integer(json, "width", &object.width, 1, 100000)
                || !integer(json, "height", &object.height, 1, 100000)
                || !json.value("customData").isObject() || !json.value("protectedReason").isString()) return false;
        object.id = json.value("id").toString(); object.kind = ObjectKind(kind);
        object.preservedCustomData = json.value("customData").toObject();
        object.protectedReason = json.value("protectedReason").toString();
        if (ids.contains(object.id)) return false;
        ids.insert(object.id); parsed.append(object);
    }
    *objects = std::move(parsed); return true;
}
QJsonObject draftMetadata(const GenerationDraft &draft) {
    const auto &source = draft.source; const auto &profile = source.profile;
    QJsonObject json{{"summary", draft.summary}, {"warnings", QJsonArray::fromStringList(draft.warnings)},
        {"hasThemeWarnings", draft.hasThemeWarnings}, {"seed", QString::number(source.arrangementSeed)},
        {"allowedTypes", int(source.allowedTypes)}, {"durationSeconds", source.audio.durationSeconds},
        {"profile", QJsonObject{{"name", profile.name}, {"rank", profile.rank},
            {"targetMinNps", profile.targetMinNps}, {"targetMaxNps", profile.targetMaxNps},
            {"maxPeakNps", profile.maxPeakNps}, {"minSameHandGapSeconds", profile.minSameHandGapSeconds},
            {"maxConnectionSpeed", profile.maxConnectionSpeed}, {"subdivision", profile.subdivision}}}};
    const auto plan = draft.arrangement ? draft.arrangement : source.arrangement;
    if (plan) json.insert("plan", plan->toJson());
    const auto &metrics = draft.metrics; QJsonArray families;
    for (int count : metrics.actionFamilyCounts) families.append(count);
    json.insert("metrics", QJsonObject{{"directional", metrics.directional}, {"dots", metrics.dots},
        {"bombs", metrics.bombs}, {"walls", metrics.walls}, {"averageNps", metrics.averageNps},
        {"peakNps", metrics.peakNps}, {"actionFamilyCounts", families},
        {"longestRepeatedPhraseRun", metrics.longestRepeatedPhraseRun}});
    return json;
}
bool readMetadata(const QJsonValue &value, const QString &key, GenerationDraft *draft) {
    if (!value.isObject()) return false;
    const auto json = value.toObject(); const auto profileJson = json.value("profile").toObject();
    auto &profile = draft->source.profile; profile.name = profileJson.value("name").toString();
    int types = 0; bool validSeed = false;
    if (profile.name != key || !integer(profileJson, "rank", &profile.rank, 1, 9)
            || !number(profileJson, "targetMinNps", &profile.targetMinNps)
            || !number(profileJson, "targetMaxNps", &profile.targetMaxNps)
            || !number(profileJson, "maxPeakNps", &profile.maxPeakNps)
            || !number(profileJson, "minSameHandGapSeconds", &profile.minSameHandGapSeconds)
            || !number(profileJson, "maxConnectionSpeed", &profile.maxConnectionSpeed)
            || !integer(profileJson, "subdivision", &profile.subdivision, 1, 64)
            || !integer(json, "allowedTypes", &types, 1, 15)
            || !number(json, "durationSeconds", &draft->source.audio.durationSeconds)
            || draft->source.audio.durationSeconds < 0 || profile.targetMinNps < 0
            || profile.targetMaxNps < profile.targetMinNps || profile.targetMaxNps > 30
            || profile.maxPeakNps < profile.targetMaxNps || profile.maxPeakNps > 100
            || profile.minSameHandGapSeconds < 0 || profile.minSameHandGapSeconds > 10
            || profile.maxConnectionSpeed <= 0 || profile.maxConnectionSpeed > 100
            || !json.value("seed").isString() || !json.value("summary").isString()
            || !json.value("warnings").isArray() || !json.value("hasThemeWarnings").isBool()) return false;
    draft->source.arrangementSeed = json.value("seed").toString().toUInt(&validSeed);
    if (!validSeed) return false;
    draft->source.allowedTypes = GeneratedTypes(types); draft->summary = json.value("summary").toString();
    draft->hasThemeWarnings = json.value("hasThemeWarnings").toBool();
    for (const auto &warning : json.value("warnings").toArray()) {
        if (!warning.isString()) return false;
        draft->warnings.append(warning.toString());
    }
    if (json.contains("plan")) {
        if (!json.value("plan").isObject()) return false;
        const auto planJson = json.value("plan").toObject(); MusicAnalysis evidence;
        evidence.audioFingerprint = planJson.value("audioFingerprint").toString();
        for (const auto &row : planJson.value("sections").toArray()) {
            MusicSegment segment; segment.id = row.toObject().value("segmentId").toString(); evidence.segments.append(segment);
        }
        SongArrangementPlan plan; QStringList errors;
        if (!SongArrangementPlanner::parsePlan(planJson, draft->source, evidence, &plan, &errors)) return false;
        const auto source = planJson.value("source");
        if (!source.isString() || (source.toString() != "ai" && source.toString() != "local")) return false;
        plan.source = source.toString(); plan.connectionIdentity = planJson.value("connectionIdentity").toString();
        draft->arrangement = std::make_shared<const SongArrangementPlan>(std::move(plan));
        draft->source.arrangement = draft->arrangement;
    }
    const auto metrics = json.value("metrics").toObject();
    auto &output = draft->metrics;
    if (!integer(metrics, "directional", &output.directional, 0, 1000000)
            || !integer(metrics, "dots", &output.dots, 0, 1000000)
            || !integer(metrics, "bombs", &output.bombs, 0, 1000000)
            || !integer(metrics, "walls", &output.walls, 0, 1000000)
            || !number(metrics, "averageNps", &output.averageNps) || output.averageNps < 0
            || !number(metrics, "peakNps", &output.peakNps) || output.peakNps < 0
            || !integer(metrics, "longestRepeatedPhraseRun", &output.longestRepeatedPhraseRun, 0, 1000000)
            || !metrics.value("actionFamilyCounts").isArray() || metrics.value("actionFamilyCounts").toArray().size() > 6) return false;
    for (const auto &count : metrics.value("actionFamilyCounts").toArray()) {
        if (!count.isDouble() || count.toDouble() < 0 || count.toDouble() > 1000000 || count.toDouble() != count.toInt()) return false;
        output.actionFamilyCounts.append(count.toInt());
    }
    return true;
}
}

struct EditorSessionController::Impl {
    struct FileHashEntry { qint64 size = -1, modified = -1; QString hash; };
    struct Record {
        QString key, targetId, baselineHash, lastAiHash, refinementHash;
        int rank = 7;
        QJsonObject resources, raw;
        quint64 baselineRevision = 0, pendingRevision = 0, lastRefinementRevision = 0;
        bool parsed = true, applied = false, refinementPending = false;
        QStringList warnings;
        GenerationDraft metadata;
        std::unique_ptr<BeatmapDocument> baseline, initial, working, before;
    };
    BeatmapDocument *formal = nullptr;
    QString documentId, key;
    View view = View::Formal;
    QHash<QString, std::shared_ptr<Record>> records;
    QJsonValue unrecognized, checkpoint;
    QJsonObject root;
    QStringList warnings;
    mutable QHash<QString, FileHashEntry> fileHashes;
    std::shared_ptr<Record> current() const { return records.value(key); }
    QJsonObject resources(QString *error) const {
        if (!formal) return {};
        return resourceIdentity(*formal, error, [this](const QString &path, QString *hashError) {
            const QFileInfo info(path); const auto absolute = info.absoluteFilePath();
            const auto size = info.size(), modified = info.lastModified().toMSecsSinceEpoch();
            const auto entry = fileHashes.constFind(absolute);
            if (info.isFile() && entry != fileHashes.constEnd() && entry->size == size && entry->modified == modified)
                return entry->hash;
            const auto hash = fileHash(path, hashError);
            if (!hash.isEmpty()) fileHashes.insert(absolute, {size, modified, hash});
            return hash;
        });
    }
    std::unique_ptr<BeatmapDocument> snapshot(const Record &record, QString *error) const {
        if (!formal) { fail(error, QStringLiteral("尚未关联正式工程。")); return {}; }
        QString targetId = record.targetId;
        bool exists = false;
        for (const auto &target : formal->difficulties()) if (target.id == targetId) exists = true;
        if (!exists) {
            const auto target = findTarget(*formal, record.key);
            targetId = target ? target->id : QString();
        }
        return formal->createEditingSnapshotForDifficulty(targetId, record.key, record.rank, error);
    }
    bool matches(const Record &record, QString *error) const {
        if (!formal || !formal->isLoaded()) return fail(error, QStringLiteral("尚未关联有效的正式工程。"));
        if (!record.parsed || !record.working) return fail(error, QStringLiteral("草稿记录损坏，只保留原记录供检查。"));
        const auto identity = resources(error);
        if (identity.isEmpty()) return false;
        if (identity != record.resources) return fail(error, QStringLiteral("音频、源媒体或时间参数已变化，草稿只读。"));
        const auto target = findTarget(*formal, record.key);
        if (!record.targetId.isEmpty() && (!target || target->id != record.targetId || target->rank != record.rank))
            return fail(error, QStringLiteral("目标难度已改变，草稿只读。"));
        if (record.targetId.isEmpty() && target) return fail(error, QStringLiteral("原先不存在的目标难度已建立，草稿只读。"));
        const auto hash = record.targetId.isEmpty() ? refinementBaselineHash({}) : formal->editingBaselineHash(record.targetId);
        if (hash != record.baselineHash) return fail(error, QStringLiteral("正式谱基线已变化，草稿只读。"));
        return true;
    }
    bool sourceMatches(const GenerationRequest &source, QString *error) const {
        if (!formal || !formal->isLoaded()) return fail(error, QStringLiteral("尚未关联正式工程。"));
        if ((!documentId.isEmpty() && !source.documentId.isEmpty() && source.documentId != documentId)
                || source.documentRevision != formal->revision() || timeJson(source.timeMap) != timeJson(formal->timeMap()))
            return fail(error, QStringLiteral("候选对应的工程或时间参数已变化，拒绝迟到结果。"));
        return true;
    }
    QJsonValue serialized() const {
        if (records.isEmpty() && !unrecognized.isUndefined()) return unrecognized;
        if (records.isEmpty() && root.isEmpty()) return QJsonArray{};
        QJsonObject output = root; output.insert("version", 2); output.insert("activeTarget", key);
        QJsonArray rows; QStringList keys = records.keys(); keys.sort();
        for (const auto &targetKey : keys) {
            const auto record = records.value(targetKey);
            if (!record->parsed || !record->working || !record->initial || !record->baseline) { rows.append(record->raw); continue; }
            QJsonObject row = record->raw;
            row.insert("target", record->key); row.insert("applied", record->applied);
            row.insert("baseline", QJsonObject{{"difficultyId", record->targetId}, {"rank", record->rank},
                {"revision", QString::number(record->baselineRevision)}, {"hash", record->baselineHash},
                {"resources", record->resources}, {"objects", objectsJson(record->baseline->objects())}});
            row.insert("initial", objectsJson(record->initial->objects()));
            row.insert("working", objectsJson(record->working->objects()));
            row.insert("beforeRefinement", record->before ? QJsonValue(objectsJson(record->before->objects())) : QJsonValue(QJsonValue::Null));
            row.insert("metadata", draftMetadata(record->metadata)); row.insert("lastAiHash", record->lastAiHash);
            rows.append(row);
        }
        output.insert("records", rows);
        if (!unrecognized.isUndefined()) output.insert("unrecognizedRecords", unrecognized);
        return output;
    }
    QJsonValue withCurrentFields(const QJsonObject &fields) const {
        auto output = serialized().toObject(); auto rows = output.value("records").toArray();
        for (int index = 0; index < rows.size(); ++index) {
            auto row = rows[index].toObject();
            if (row.value("target").toString() != key) continue;
            for (auto field = fields.constBegin(); field != fields.constEnd(); ++field) row.insert(field.key(), field.value());
            rows.replace(index, row); break;
        }
        output.insert("records", rows); return output;
    }
    bool canPersist(const QJsonValue &value, QString *error) const {
        if (!formal || !formal->isLoaded() || formal->isEditingSnapshot())
            return fail(error, QStringLiteral("草稿记录仅随正式工程保存。"));
        if (QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).size() > 32 * 1024 * 1024)
            return fail(error, QStringLiteral("编辑草稿记录超过 32 MiB 保存上限，未执行操作。"));
        return true;
    }
};

EditorSessionController::EditorSessionController(QObject *parent) : QObject(parent), d(new Impl) {}
EditorSessionController::~EditorSessionController() = default;
void EditorSessionController::setFormalDocument(BeatmapDocument *document, const QString &documentId) {
    d->formal = document; d->documentId = documentId; d->key.clear(); d->records.clear(); d->root = {};
    d->warnings.clear(); d->unrecognized = QJsonValue(QJsonValue::Undefined); d->view = View::Formal;
    d->fileHashes.clear();
    restoreFromJson(document ? document->editorDraftRecords() : QJsonValue(QJsonValue::Undefined));
    if (d->key.isEmpty() && document) for (const auto &difficulty : document->difficulties())
        if (difficulty.id == document->currentDifficultyId()) { d->key = difficulty.name; break; }
    emit changed();
}
BeatmapDocument *EditorSessionController::formalDocument() const { return d->formal; }
BeatmapDocument *EditorSessionController::workingDocument() const { const auto r = d->current(); return r ? r->working.get() : nullptr; }
const BeatmapDocument *EditorSessionController::initialDocument() const { const auto r = d->current(); return r ? r->initial.get() : nullptr; }
const BeatmapDocument *EditorSessionController::beforeRefinementDocument() const { const auto r = d->current(); return r ? r->before.get() : nullptr; }
BeatmapDocument *EditorSessionController::activeDocument() const {
    const auto record = d->current();
    if (record) {
        if (d->view == View::Formal && record->baseline && d->formal && !findTarget(*d->formal, record->key))
            return record->baseline.get();
        if (d->view == View::Working && record->working) return record->working.get();
        if (d->view == View::Initial && record->initial) return record->initial.get();
        if (d->view == View::BeforeRefinement && record->before) return record->before.get();
    }
    return d->formal;
}
QString EditorSessionController::currentTargetKey() const { return d->key; }
QStringList EditorSessionController::targetKeys() const { auto keys = d->records.keys(); keys.sort(); return keys; }
bool EditorSessionController::selectTarget(const QString &key, QString *error) {
    if (key.trimmed().isEmpty() || key.size() > 128) return fail(error, QStringLiteral("目标难度名称无效。"));
    d->key = key; d->view = d->current() && d->current()->working && !d->current()->applied ? View::Working : View::Formal;
    emit changed(); return true;
}
EditorSessionController::View EditorSessionController::view() const { return d->view; }
bool EditorSessionController::setView(View view, QString *error) {
    const auto record = d->current();
    if ((view == View::Working && (!record || !record->working))
            || (view == View::Initial && (!record || !record->initial))
            || (view == View::BeforeRefinement && (!record || !record->before)))
        return fail(error, QStringLiteral("这个对比版本尚不存在。"));
    if (view != View::Formal && view != View::Working && view != View::Initial && view != View::BeforeRefinement)
        return fail(error, QStringLiteral("不支持的对比版本。"));
    if (view == View::Formal && d->formal && record) {
        const auto target = findTarget(*d->formal, record->key);
        if (target && target->id != d->formal->currentDifficultyId() && !d->formal->setDifficulty(target->id, error)) return false;
    }
    d->view = view; emit changed(); return true;
}
bool EditorSessionController::viewReadOnly() const {
    if (d->view == View::Initial || d->view == View::BeforeRefinement) return true;
    if (d->view == View::Formal) {
        const auto record = d->current();
        return record && d->formal && !findTarget(*d->formal, record->key);
    }
    return !isApplicable();
}
bool EditorSessionController::hasDraft() const { return bool(d->current()); }
bool EditorSessionController::hasBeforeRefinement() const { return beforeRefinementDocument() != nullptr; }
bool EditorSessionController::isApplied() const { const auto r = d->current(); return r && r->applied; }
bool EditorSessionController::isManualModified() const {
    const auto r = d->current(); return r && r->working && refinementBaselineHash(r->working->objects()) != r->lastAiHash;
}
bool EditorSessionController::isApplicable(QString *reason) const {
    const auto record = d->current();
    if (!record) return fail(reason, QStringLiteral("当前难度没有待应用草稿。"));
    if (record->applied) return fail(reason, QStringLiteral("草稿已应用，保留版本仅供对比。"));
    return d->matches(*record, reason);
}
QStringList EditorSessionController::warnings() const {
    auto output = d->warnings; const auto record = d->current();
    if (record) {
        output.append(record->warnings);
        QString reason; if (!record->applied && !d->matches(*record, &reason)) output.append(reason);
    }
    output.removeDuplicates(); return output;
}
GenerationDraft EditorSessionController::generationDraft() const {
    const auto r = d->current(); if (!r) return {};
    auto draft = r->metadata; if (r->working) draft.objects = r->working->objects(); return draft;
}
bool EditorSessionController::acceptGenerationDraft(const GenerationDraft &draft, QString *error) {
    if (!d->sourceMatches(draft.source, error)) return false;
    if (!d->formal->isNewSong()) return fail(error, QStringLiteral("生成草稿只支持本软件新建工程。"));
    const auto key = draft.source.profile.name;
    if (key.isEmpty() || key.size() > 128 || draft.source.profile.rank < 1 || draft.source.profile.rank > 9)
        return fail(error, QStringLiteral("候选的目标难度无效。"));
    auto record = std::make_shared<Impl::Record>(); record->key = key; record->rank = draft.source.profile.rank;
    const auto target = findTarget(*d->formal, key); record->targetId = target ? target->id : QString();
    if (target && target->rank != record->rank) return fail(error, QStringLiteral("候选难度与正式目标不一致。"));
    record->resources = d->resources(error); if (record->resources.isEmpty()) return false;
    record->baseline = d->snapshot(*record, error); if (!record->baseline) return false;
    record->baselineHash = record->baseline->editingBaselineHash(); record->baselineRevision = d->formal->revision();
    record->working = d->snapshot(*record, error); if (!record->working) return false;
    if (!record->working->replaceEditingSnapshotObjects(draft.objects, true, error)) return false;
    record->initial = record->working->createEditingSnapshot(error); if (!record->initial) return false;
    record->metadata = draft; record->metadata.objects = record->working->objects();
    record->lastAiHash = refinementBaselineHash(record->working->objects());
    const auto previousRecord = d->records.value(key); const auto previousKey = d->key; const auto previousView = d->view;
    d->records.insert(key, record); d->key = key; d->view = View::Working;
    if (!synchronizeRecords(error)) {
        if (previousRecord) d->records.insert(key, previousRecord); else d->records.remove(key);
        d->key = previousKey; d->view = previousView; return false;
    }
    emit changed(); return true;
}
bool EditorSessionController::beginRefinementDraft(const GenerationRequest &source, QString *error) {
    if (!d->sourceMatches(source, error)) return false;
    if (!d->formal->isNewSong()) return fail(error, QStringLiteral("精修草稿只支持本软件新建工程。"));
    const auto target = findTarget(*d->formal, source.profile.name);
    if (!target) return fail(error, QStringLiteral("正式谱中没有这个目标难度。"));
    if (const auto existing = d->records.value(source.profile.name)) {
        if (!existing->applied) { d->key = source.profile.name; if (!isApplicable(error)) return false; d->view = View::Working; emit changed(); return true; }
    }
    auto snapshot = d->formal->createEditingSnapshotForDifficulty(target->id, {}, target->rank, error);
    if (!snapshot) return false;
    GenerationDraft draft; draft.source = source; draft.objects = snapshot->objects();
    draft.summary = QStringLiteral("由正式谱建立的精修草稿；确认应用前正式谱保持原状。");
    return acceptGenerationDraft(draft, error);
}
bool EditorSessionController::beginRefinement(bool continuation, QString *error) {
    if (!isApplicable(error)) return false;
    const auto record = d->current();
    std::unique_ptr<BeatmapDocument> before;
    if (continuation) {
        if (record->refinementHash.isEmpty() || record->lastRefinementRevision != record->working->revision())
            return fail(error, QStringLiteral("草稿已手动变化或没有可继续的会话，请重新发起精修。"));
    } else {
        before = record->working->createEditingSnapshot(error); if (!before) return false;
    }
    const auto proposed = before ? d->withCurrentFields({{"beforeRefinement", objectsJson(before->objects())}}) : toJson();
    if (!d->canPersist(proposed, error)) return false;
    if (before) { record->before = std::move(before); record->refinementHash = refinementBaselineHash(record->working->objects()); }
    record->pendingRevision = record->working->revision(); record->refinementPending = true;
    if (!d->formal->setEditorDraftRecords(proposed, error)) return false;
    emit changed(); return true;
}
bool EditorSessionController::mergeRefinementResult(const RefinementResult &result, QString *error) {
    if (!isApplicable(error)) return false;
    const auto record = d->current();
    if (!record->refinementPending || record->working->revision() != record->pendingRevision
            || result.source.baselineHash != record->refinementHash
            || refinementBaselineHash(result.source.baseline) != record->refinementHash
            || result.source.generation.profile.name != record->key
            || (!d->documentId.isEmpty() && !result.source.generation.documentId.isEmpty()
                && result.source.generation.documentId != d->documentId))
        return fail(error, QStringLiteral("精修来源或工作草稿已变化，拒绝迟到结果。"));
    auto staged = record->working->createEditingSnapshot(error);
    if (!staged || !staged->replaceEditingSnapshotObjects(result.candidate.objects, true, error)) return false;
    const auto plan = record->metadata.arrangement; const auto source = record->metadata.source;
    auto metadata = result.candidate; metadata.source = source;
    if (!metadata.arrangement) metadata.arrangement = plan;
    metadata.objects = staged->objects();
    const auto proposed = d->withCurrentFields({{"working", objectsJson(metadata.objects)},
        {"metadata", draftMetadata(metadata)}, {"lastAiHash", refinementBaselineHash(metadata.objects)}});
    if (!d->canPersist(proposed, error)) return false;
    if (!record->working->replaceEditingSnapshotObjects(metadata.objects, false, error)) return false;
    record->metadata = std::move(metadata);
    record->lastAiHash = refinementBaselineHash(record->working->objects());
    record->lastRefinementRevision = record->working->revision();
    // Services may emit another passed partial result without a new begin call.
    record->pendingRevision = record->working->revision();
    d->view = View::Working;
    if (!d->formal->setEditorDraftRecords(proposed, error)) return false;
    emit changed(); return true;
}
bool EditorSessionController::restoreInitial(QString *error) {
    if (!isApplicable(error)) return false;
    const auto record = d->current();
    if (!record->initial) return fail(error, QStringLiteral("这个草稿没有初稿版本。"));
    auto staged = record->working->createEditingSnapshot(error);
    if (!staged || !staged->replaceEditingSnapshotObjects(record->initial->objects(), true, error)) return false;
    const auto proposed = d->withCurrentFields({{"working", objectsJson(staged->objects())}});
    if (!d->canPersist(proposed, error)) return false;
    if (!record->working->replaceEditingSnapshotObjects(staged->objects(), false, error)) return false;
    record->refinementPending = false; record->refinementHash.clear(); d->view = View::Working;
    if (!d->formal->setEditorDraftRecords(proposed, error)) return false;
    emit changed(); return true;
}
bool EditorSessionController::applyDraft(QString *error) {
    if (!isApplicable(error)) return false;
    const auto record = d->current(); EditingDraftApplication application;
    application.targetDifficultyId = record->targetId; application.targetDifficultyName = record->key;
    application.targetDifficultyRank = record->rank; application.baselineHash = record->baselineHash;
    application.expectedRevision = d->formal->revision(); application.objects = record->working->objects();
    const auto proposed = d->withCurrentFields({{"applied", true}});
    if (!d->canPersist(proposed, error)) return false;
    if (!d->formal->applyEditingDraft(application, error)) return false;
    record->applied = true; record->refinementPending = false; record->refinementHash.clear(); d->view = View::Formal;
    if (!d->formal->setEditorDraftRecords(proposed, error)) return false;
    emit changed(); return true;
}
bool EditorSessionController::discardDraft(QString *error) {
    const auto previous = d->records.value(d->key); const auto previousView = d->view;
    if (!previous) return fail(error, QStringLiteral("当前难度没有草稿。"));
    d->records.remove(d->key);
    d->view = View::Formal;
    if (!synchronizeRecords(error)) { d->records.insert(d->key, previous); d->view = previousView; return false; }
    emit changed(); return true;
}
bool EditorSessionController::canUndo() const { return d->view == View::Working && !viewReadOnly() && workingDocument() && workingDocument()->canUndo(); }
bool EditorSessionController::canRedo() const { return d->view == View::Working && !viewReadOnly() && workingDocument() && workingDocument()->canRedo(); }
bool EditorSessionController::undo() { if (!canUndo() || !workingDocument()->undo()) return false; notifyWorkingChanged(); return true; }
bool EditorSessionController::redo() { if (!canRedo() || !workingDocument()->redo()) return false; notifyWorkingChanged(); return true; }
void EditorSessionController::notifyWorkingChanged() {
    const auto record = d->current();
    if (!record || !record->working || record->applied || d->view != View::Working) return;
    record->refinementPending = false; record->refinementHash.clear(); synchronizeRecords(); emit changed();
}
QJsonValue EditorSessionController::toJson() const { return d->serialized(); }
bool EditorSessionController::restoreFromJson(const QJsonValue &records, QStringList *outputWarnings) {
    d->records.clear(); d->warnings.clear(); d->root = {}; d->key.clear(); d->view = View::Formal;
    d->unrecognized = QJsonValue(QJsonValue::Undefined);
    if (records.isNull() || records.isUndefined() || (records.isArray() && records.toArray().isEmpty())) {
        d->checkpoint = toJson(); return true;
    }
    if (!records.isObject() || records.toObject().value("version").toDouble() != 2
            || !records.toObject().value("records").isArray()) {
        d->unrecognized = records; d->warnings.append(QStringLiteral("草稿记录版本或结构无法读取，已保留原记录，正式谱仍可打开。"));
        if (outputWarnings) outputWarnings->append(d->warnings); d->checkpoint = toJson(); return false;
    }
    d->root = records.toObject(); d->unrecognized = d->root.value("unrecognizedRecords");
    QJsonArray rejected;
    for (const auto &value : d->root.value("records").toArray()) {
        const auto row = value.toObject(); const auto key = row.value("target").toString();
        if (!value.isObject() || key.isEmpty() || key.size() > 128 || d->records.contains(key)) {
            if (d->records.contains(key)) {
                d->records.value(key)->parsed = false;
                d->records.value(key)->warnings.append(QStringLiteral("目标难度存在重复草稿记录，禁止应用。"));
            }
            rejected.append(value); d->warnings.append(QStringLiteral("存在无法识别或重复的草稿记录，已保留且不会应用。")); continue;
        }
        auto record = std::make_shared<Impl::Record>(); record->key = key; record->raw = row;
        const auto baseline = row.value("baseline").toObject(); bool revisionValid = false;
        record->targetId = baseline.value("difficultyId").toString(); record->baselineHash = baseline.value("hash").toString();
        record->baselineRevision = baseline.value("revision").toString().toULongLong(&revisionValid);
        record->resources = baseline.value("resources").toObject();
        record->applied = row.value("applied").toBool(); record->lastAiHash = row.value("lastAiHash").toString();
        if (d->formal) record->metadata.source.timeMap = d->formal->timeMap();
        QVector<BeatObject> original, initial, working, before;
        record->parsed = baseline.value("difficultyId").isString() && baseline.value("hash").isString()
            && !record->baselineHash.isEmpty() && revisionValid && integer(baseline, "rank", &record->rank, 1, 9)
            && !record->resources.isEmpty() && row.value("applied").isBool() && row.value("lastAiHash").isString()
            && readObjects(baseline.value("objects"), &original) && refinementBaselineHash(original) == record->baselineHash
            && readObjects(row.value("initial"), &initial) && readObjects(row.value("working"), &working)
            && (row.value("beforeRefinement").isNull() || readObjects(row.value("beforeRefinement"), &before))
            && readMetadata(row.value("metadata"), key, &record->metadata);
        QString error;
        if (record->parsed) {
            record->baseline = d->snapshot(*record, &error); record->initial = d->snapshot(*record, &error);
            record->working = d->snapshot(*record, &error);
            record->parsed = record->baseline && record->initial && record->working
                && record->baseline->replaceEditingSnapshotObjects(original, true, &error)
                && record->initial->replaceEditingSnapshotObjects(initial, true, &error)
                && record->working->replaceEditingSnapshotObjects(working, true, &error);
            if (record->parsed && !row.value("beforeRefinement").isNull()) {
                record->before = d->snapshot(*record, &error);
                record->parsed = record->before && record->before->replaceEditingSnapshotObjects(before, true, &error);
            }
            record->metadata.source.documentId = d->documentId;
            record->metadata.source.documentRevision = d->formal ? d->formal->revision() : 0;
            record->metadata.source.difficultyId = record->working ? record->working->currentDifficultyId() : record->targetId;
            if (d->formal) record->metadata.source.timeMap = d->formal->timeMap();
            record->metadata.objects = working;
        }
        if (!record->parsed) record->warnings.append(QStringLiteral("%1 草稿记录损坏，已保留原数据并禁止应用。%2").arg(key, error));
        else if (!record->applied && !d->matches(*record, &error)) record->warnings.append(error);
        d->records.insert(key, record);
    }
    if (!rejected.isEmpty()) {
        if (!d->unrecognized.isUndefined()) rejected.prepend(d->unrecognized);
        d->unrecognized = rejected;
    }
    d->key = d->root.value("activeTarget").toString();
    if (!d->records.contains(d->key) && !d->records.isEmpty()) d->key = targetKeys().first();
    const auto current = d->current(); if (current && current->working && !current->applied) d->view = View::Working;
    d->checkpoint = toJson();
    if (outputWarnings) outputWarnings->append(warnings());
    emit changed(); return d->warnings.isEmpty();
}
bool EditorSessionController::synchronizeRecords(QString *error) {
    const auto records = toJson();
    if (!d->canPersist(records, error)) return false;
    return d->formal->setEditorDraftRecords(records, error);
}
bool EditorSessionController::isDirty() const { return toJson() != d->checkpoint; }
void EditorSessionController::markSaved() { d->checkpoint = toJson(); }
QJsonValue EditorSessionController::serializedCheckpoint() const { return d->checkpoint; }

} // namespace lmsc
