#include "SongArrangementPlan.h"
#include "AiGenerationService.h"
#include "MusicFeatureAnalyzer.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>

namespace lmsc {
namespace {
const QStringList families{"vertical", "diagonal", "horizontal", "staircase", "expansion", "questionAnswer"};
const QStringList rhythms{"balanced", "strongBeats", "syncopated", "sparse"};
const QStringList amplitudes{"compact", "medium", "wide"};
const QStringList developments{"introduce", "repeat", "vary", "contrast", "build", "close"};
const QStringList familyLabels{QStringLiteral("垂直收放"), QStringLiteral("斜切收放"), QStringLiteral("横向往返"),
    QStringLiteral("上下阶梯"), QStringLiteral("内外展开"), QStringLiteral("左右问答")};
const QStringList developmentLabels{QStringLiteral("引入"), QStringLiteral("再现"), QStringLiteral("变奏"),
    QStringLiteral("对比"), QStringLiteral("推进"), QStringLiteral("收束")};
quint32 mix(quint32 value) {
    value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15;
    value *= 0x846ca68bu; return value ^ (value >> 16);
}
quint32 fingerprintSeed(const QString &fingerprint, quint32 seed) {
    quint32 value = 2166136261u ^ seed;
    for (QChar ch : fingerprint) value = (value ^ ch.unicode()) * 16777619u;
    return value;
}
QJsonObject enumeration(const QStringList &values) {
    return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}};
}
QJsonObject objectSchema(const QJsonObject &properties, const QStringList &required) {
    return {{"type", "object"}, {"properties", properties},
        {"required", QJsonArray::fromStringList(required)}, {"additionalProperties", false}};
}
bool text(const QJsonObject &json, const char *key, QString *value, int maximum = 128, bool allowEmpty = false) {
    const auto item = json.value(QLatin1String(key));
    if (!item.isString()) return false;
    *value = item.toString();
    return value->size() <= maximum && (allowEmpty || !value->trimmed().isEmpty());
}
bool exactKeys(const QJsonObject &json, const QStringList &keys) {
    for (auto it = json.constBegin(); it != json.constEnd(); ++it) if (!keys.contains(it.key())) return false;
    return true;
}
}

const SectionArrangement *SongArrangementPlan::section(const QString &id) const {
    for (const auto &item : sections) if (item.segmentId == id) return &item;
    return nullptr;
}

QJsonObject SongArrangementPlan::toJson() const {
    QJsonArray blocks;
    for (const auto &item : sections) blocks.append(QJsonObject{{"segmentId", item.segmentId},
        {"motifId", item.motifId}, {"role", item.role}, {"rest", item.rest}, {"targetNps", item.targetNps},
        {"family", families.value(int(item.family))}, {"rhythm", rhythms.value(int(item.rhythm))},
        {"amplitude", amplitudes.value(int(item.amplitude))}, {"development", developments.value(int(item.development))}});
    return {{"schemaVersion", 2}, {"summary", summary}, {"audioFingerprint", audioFingerprint},
        {"source", source}, {"connectionIdentity", connectionIdentity}, {"sections", blocks}};
}

QString SongArrangementPlan::description() const {
    QStringList lines{summary, source == "ai" ? QStringLiteral("规划来源：AI 整曲建议") : QStringLiteral("规划来源：本地音乐分析")};
    for (const auto &item : sections) lines.append(QStringLiteral("%1 · %2 · %3 · %4 · %5 NPS")
        .arg(item.segmentId, item.role, familyLabels.value(int(item.family)),
            item.rest ? QStringLiteral("留白") : developmentLabels.value(int(item.development)))
        .arg(item.targetNps, 0, 'f', 1));
    return lines.join('\n');
}

SongArrangementPlan SongArrangementPlanner::localPlan(const GenerationRequest &request, const MusicAnalysis &analysis) {
    SongArrangementPlan plan;
    plan.audioFingerprint = analysis.audioFingerprint;
    plan.source = QStringLiteral("local");
    plan.summary = QStringLiteral("随重音、起音疏密、切分、频带比例和能量发展安排短乐句；重复段保留主题核心并变奏，留白处收束。频带仅作编排线索。");
    double maximumEnergy = 0;
    for (const auto &segment : analysis.segments) maximumEnergy = qMax(maximumEnergy, segment.energy);
    const quint32 seed = fingerprintSeed(analysis.audioFingerprint, request.arrangementSeed);
    QHash<QString, int> occurrences;
    QVector<int> recent;
    for (int index = 0; index < analysis.segments.size(); ++index) {
        const auto &segment = analysis.segments[index];
        double low = 0, mid = 0, high = 0, sync = 0, trend = 0, rest = 0, gaps = 0;
        int count = 0;
        for (const auto &phrase : analysis.phrases) if (phrase.segmentIndex == index) {
            low += phrase.low; mid += phrase.mid; high += phrase.high; sync += phrase.syncopation;
            trend += phrase.energyTrend; rest += phrase.restFraction; gaps += phrase.longestGapBeats; ++count;
        }
        const double divisor = qMax(1, count);
        low /= divisor; mid /= divisor; high /= divisor; sync /= divisor; trend /= divisor;
        rest /= divisor; gaps /= divisor;
        const double energy = qBound(0.0, segment.energy / qMax(1e-12, maximumEnergy), 1.0);
        std::array<double, 6> scores{{.4 + low * .5 + rest * .4, .5 + mid * .65 + sync * .4,
            .3 + high * .7, .4 + qMax(0.0, trend) * .8 + energy * .2,
            .4 + energy * .4 + low * .3, .4 + sync * .6 + qMin(1.0, gaps / 4) * .2}};
        for (int family = 0; family < 6; ++family) {
            scores[family] += (mix(seed ^ quint32(index * 131 + family * 977)) % 1000) / 1000.0 * .5;
            for (int age = 0; age < recent.size(); ++age)
                if (recent[recent.size() - 1 - age] == family) scores[family] -= .85 / (age + 1);
            if (request.profile.rank <= 3 && (family == 2 || family == 3)) scores[family] -= .15;
        }
        const int family = int(std::max_element(scores.begin(), scores.end()) - scores.begin());
        const QString group = segment.repeatGroup.isEmpty() ? segment.id : segment.repeatGroup;
        const int occurrence = occurrences[group]++;
        SectionArrangement item;
        item.segmentId = segment.id; item.motifId = group;
        item.family = ActionFamily(family);
        item.rest = segment.activeSeconds <= .01 || segment.energy <= 1e-6;
        item.targetNps = item.rest ? 0 : request.profile.targetMinNps
            + (request.profile.targetMaxNps - request.profile.targetMinNps) * (.15 + .75 * std::sqrt(energy));
        item.rhythm = rest > .4 ? RhythmStrategy::Sparse : sync > .25 ? RhythmStrategy::Syncopated
            : low > .5 ? RhythmStrategy::StrongBeats : RhythmStrategy::Balanced;
        item.amplitude = request.profile.rank == 1 ? SpaceAmplitude::Compact
            : request.profile.rank <= 3 || energy < .25 ? SpaceAmplitude::Medium : SpaceAmplitude::Wide;
        item.development = occurrence == 0 ? ThemeDevelopment::Introduce
            : occurrence % 4 == 1 ? ThemeDevelopment::Vary : occurrence % 4 == 2 ? ThemeDevelopment::Build
            : occurrence % 4 == 3 ? ThemeDevelopment::Contrast : ThemeDevelopment::Repeat;
        if (trend > .1 && !item.rest) item.development = ThemeDevelopment::Build;
        if (index == analysis.segments.size() - 1) item.development = ThemeDevelopment::Close;
        item.role = item.rest ? QStringLiteral("留白") : index == 0 ? QStringLiteral("开场")
            : item.development == ThemeDevelopment::Build ? QStringLiteral("推进段")
            : item.development == ThemeDevelopment::Close ? QStringLiteral("收尾")
            : occurrence ? QStringLiteral("主题发展") : QStringLiteral("对比段");
        plan.sections.append(item);
        recent.append(family); if (recent.size() > 4) recent.removeFirst();
    }
    return plan;
}

QJsonObject SongArrangementPlanner::outputSchema(const MusicAnalysis &analysis) {
    QStringList ids;
    for (const auto &segment : analysis.segments) ids.append(segment.id);
    const QJsonObject shortText{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
    const auto section = objectSchema({{"segmentId", enumeration(ids)}, {"motifId", shortText},
        {"role", shortText}, {"rest", QJsonObject{{"type", "boolean"}}},
        {"targetNps", QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 10}}},
        {"family", enumeration(families)}, {"rhythm", enumeration(rhythms)},
        {"amplitude", enumeration(amplitudes)}, {"development", enumeration(developments)}},
        {"segmentId", "motifId", "role", "rest", "targetNps", "family", "rhythm", "amplitude", "development"});
    return objectSchema({{"schemaVersion", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{2}}}},
        {"summary", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 2000}}},
        {"audioFingerprint", enumeration({analysis.audioFingerprint})},
        {"sections", QJsonObject{{"type", "array"}, {"minItems", analysis.segments.size()},
            {"maxItems", analysis.segments.size()}, {"items", section}}}},
        {"schemaVersion", "summary", "audioFingerprint", "sections"});
}

bool SongArrangementPlanner::parsePlan(const QJsonObject &json, const GenerationRequest &request,
                                       const MusicAnalysis &analysis, SongArrangementPlan *plan, QStringList *errors) {
    QStringList localErrors;
    if (!errors) errors = &localErrors;
    errors->clear();
    if (!plan) { errors->append(QStringLiteral("没有提供整曲规划容器。")); return false; }
    *plan = {};
    SongArrangementPlan parsed;
    if (!exactKeys(json, {"schemaVersion", "summary", "audioFingerprint", "sections", "source", "connectionIdentity"})
            || !json.value("schemaVersion").isDouble() || json.value("schemaVersion").toDouble() != 2
            || !text(json, "summary", &parsed.summary, 2000)
            || !text(json, "audioFingerprint", &parsed.audioFingerprint, 128, true)
            || parsed.audioFingerprint != analysis.audioFingerprint || !json.value("sections").isArray()) {
        errors->append(QStringLiteral("整曲建议格式或音频指纹无效，必须使用当前音乐证据的 schemaVersion 2。")); return false;
    }
    const QJsonArray blocks = json.value("sections").toArray();
    if (blocks.size() != analysis.segments.size() || blocks.size() > 10000) {
        errors->append(QStringLiteral("整曲建议必须对每个分析段落提供且仅提供一次安排。")); return false;
    }
    QHash<QString, SectionArrangement> byId;
    for (const auto &value : blocks) {
        if (!value.isObject()) { errors->append(QStringLiteral("段落建议必须为对象。")); break; }
        const auto itemJson = value.toObject();
        SectionArrangement item;
        QString family, rhythm, amplitude, development;
        if (!exactKeys(itemJson, {"segmentId", "motifId", "role", "rest", "targetNps", "family", "rhythm", "amplitude", "development"})
                || !text(itemJson, "segmentId", &item.segmentId) || !text(itemJson, "motifId", &item.motifId)
                || !text(itemJson, "role", &item.role) || !itemJson.value("rest").isBool()
                || !itemJson.value("targetNps").isDouble() || !text(itemJson, "family", &family)
                || !text(itemJson, "rhythm", &rhythm) || !text(itemJson, "amplitude", &amplitude)
                || !text(itemJson, "development", &development) || !families.contains(family)
                || !rhythms.contains(rhythm) || !amplitudes.contains(amplitude) || !developments.contains(development)) {
            errors->append(QStringLiteral("段落建议包含无效字段或未支持的编排选项。")); break;
        }
        item.rest = itemJson.value("rest").toBool(); item.targetNps = itemJson.value("targetNps").toDouble();
        if (!std::isfinite(item.targetNps) || item.targetNps < 0 || item.targetNps > request.profile.targetMaxNps + 1e-7
                || (item.rest && item.targetNps > 1e-7) || byId.contains(item.segmentId)) {
            errors->append(QStringLiteral("段落密度超出当前难度或重复提供段落。")); break;
        }
        item.family = ActionFamily(families.indexOf(family)); item.rhythm = RhythmStrategy(rhythms.indexOf(rhythm));
        item.amplitude = SpaceAmplitude(amplitudes.indexOf(amplitude));
        item.development = ThemeDevelopment(developments.indexOf(development));
        byId.insert(item.segmentId, item);
    }
    if (errors->isEmpty()) for (const auto &segment : analysis.segments) {
        if (!byId.contains(segment.id)) { errors->append(QStringLiteral("整曲建议引用了缺失或未知段落。")); break; }
        parsed.sections.append(byId.value(segment.id));
    }
    if (!errors->isEmpty()) return false;
    parsed.source = QStringLiteral("ai");
    *plan = std::move(parsed);
    return true;
}
} // namespace lmsc
