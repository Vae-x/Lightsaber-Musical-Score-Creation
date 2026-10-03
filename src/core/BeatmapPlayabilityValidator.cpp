#include "BeatmapPlayabilityValidator.h"

#include <QPointF>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace lmsc {
namespace {
constexpr double epsilon = 1e-7;
constexpr double bombWindowSeconds = 0.2;
constexpr double continuousMovementSeconds = 1.0;
struct HandConnection {
    double startSeconds, endSeconds;
    QPointF exit, entry;
};
bool integer(const QJsonObject &json, const char *key, int *value) {
    const auto item = json.value(QLatin1String(key));
    if (!item.isDouble()) return false;
    const double number = item.toDouble();
    if (!std::isfinite(number) || number < -100000 || number > 100000 || number != std::floor(number)) return false;
    *value = static_cast<int>(number); return true;
}
bool exactKeys(const QJsonObject &json, const QStringList &allowed) {
    for (auto it = json.constBegin(); it != json.constEnd(); ++it) if (!allowed.contains(it.key())) return false;
    return true;
}
QPointF cutVector(int direction) {
    const double diagonal = std::sqrt(0.5);
    const QPointF vectors[] = {{0,1}, {0,-1}, {-1,0}, {1,0}, {-diagonal,diagonal},
                               {diagonal,diagonal}, {-diagonal,-diagonal}, {diagonal,-diagonal}, {0,0}};
    return direction >= 0 && direction <= 8 ? vectors[direction] : QPointF{};
}
double length(const QPointF &point) { return std::hypot(point.x(), point.y()); }
QPointF centre(const BeatObject &object) { return {double(object.x), double(object.y)}; }
double pointToSegment(const QPointF &point, const QPointF &start, const QPointF &end) {
    const QPointF delta = end-start;
    const double square = delta.x()*delta.x() + delta.y()*delta.y();
    const QPointF diff = point-start;
    const double t = square < epsilon ? 0.0 : qBound(0.0, (diff.x()*delta.x()+diff.y()*delta.y())/square, 1.0);
    return length(point-(start+delta*t));
}
double pointToCut(const QPointF &point, const BeatObject &note) {
    const QPointF vector = cutVector(note.direction) * 0.35;
    return pointToSegment(point, centre(note)-vector, centre(note)+vector);
}
void addError(QStringList *errors, const QString &error) { if (errors && errors->size() < 20) errors->append(error); }
QVector<BeatObject> orderedNotes(const QVector<BeatObject> &source) {
    QVector<BeatObject> result;
    for (const auto &object : source) if (object.kind == ObjectKind::Note) result.append(object);
    std::sort(result.begin(), result.end(), [](const BeatObject &a, const BeatObject &b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        if (a.color != b.color) return a.color < b.color;
        if (a.direction != b.direction) return a.direction < b.direction;
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
    });
    return result;
}
}

GenerationMetrics BeatmapPlayabilityValidator::metrics(const QVector<BeatObject> &objects,
                                                       const TimeMap &timeMap, double activeSeconds) {
    GenerationMetrics result;
    QVector<double> times;
    for (const auto &object : objects) {
        if (object.kind == ObjectKind::Note) {
            object.direction == 8 ? ++result.dots : ++result.directional;
            times.append(timeMap.beatToSeconds(object.beat));
        } else if (object.kind == ObjectKind::Bomb) ++result.bombs;
        else if (object.kind == ObjectKind::Wall) ++result.walls;
    }
    std::sort(times.begin(), times.end());
    int left = 0;
    for (int right = 0; right < times.size(); ++right) {
        while (left < right && times[right]-times[left] >= 2.0-epsilon) ++left;
        result.peakNps = qMax(result.peakNps, (right-left+1)/2.0);
    }
    result.averageNps = activeSeconds > 0 ? times.size()/activeSeconds : 0;
    return result;
}

bool BeatmapPlayabilityValidator::validateObjects(const QVector<BeatObject> &objects,
                                                 const GenerationRequest &request,
                                                 const MusicAnalysis &analysis, QStringList *errors) {
    QStringList localErrors;
    if (!errors) errors = &localErrors;
    errors->clear();
    QVector<BeatObject> notes, bombs, walls;
    QSet<QString> occupied;
    for (const auto &object : objects) {
        if (!std::isfinite(object.beat) || object.beat < 0 || object.x < 0 || object.x > 3
                || object.y < 0 || object.y > 2 || object.isProtected()) {
            addError(errors, QStringLiteral("物件位置、时间或保护状态无效。")); continue;
        }
        const double seconds = request.timeMap.beatToSeconds(object.beat);
        if (seconds < -epsilon || seconds >= analysis.durationSeconds)
            addError(errors, QStringLiteral("物件超出音频时间范围。"));
        if (object.kind == ObjectKind::Note) {
            if (object.color < 0 || object.color > 1 || object.direction < 0 || object.direction > 8)
                addError(errors, QStringLiteral("音符左右手或方向无效。"));
            if ((object.direction == 8 && !request.allowedTypes.testFlag(DotType))
                    || (object.direction != 8 && !request.allowedTypes.testFlag(DirectionalType)))
                addError(errors, QStringLiteral("返回了未选择的音符类型。"));
            if ((object.color == 0 && object.x > 1) || (object.color == 1 && object.x < 2))
                addError(errors, QStringLiteral("第 %1 拍要求交叉手，首版顺手模式禁止。 ").arg(object.beat).trimmed());
            notes.append(object);
        } else if (object.kind == ObjectKind::Bomb) {
            if (!request.allowedTypes.testFlag(BombType)) addError(errors, QStringLiteral("返回了未选择的炸弹。"));
            bombs.append(object);
        } else if (object.kind == ObjectKind::Wall) {
            if (!request.allowedTypes.testFlag(WallType)) addError(errors, QStringLiteral("返回了未选择的墙。"));
            const double end = request.timeMap.beatToSeconds(object.beat+object.duration);
            if (!std::isfinite(object.duration) || object.duration <= 0 || (object.x != 0 && object.x != 3)
                    || object.width != 1 || object.y != 0 || object.height != 5
                    || end-seconds < 0.5-epsilon || end-seconds > 2.0+epsilon || end > analysis.durationSeconds+epsilon)
                addError(errors, QStringLiteral("墙必须为单列侧墙，持续 0.5–2 秒且位于音频范围内。"));
            walls.append(object);
        } else addError(errors, QStringLiteral("未知物件类型。"));
        if (object.kind != ObjectKind::Wall) {
            const QString cell = QStringLiteral("%1:%2:%3").arg(qRound64(object.beat*1000000)).arg(object.x).arg(object.y);
            if (occupied.contains(cell)) addError(errors, QStringLiteral("同一拍格位置存在重叠物件。"));
            occupied.insert(cell);
        }
    }
    auto order = [](const BeatObject &a, const BeatObject &b) { return a.beat < b.beat; };
    std::stable_sort(notes.begin(), notes.end(), order);
    std::sort(bombs.begin(), bombs.end(), order); std::sort(walls.begin(), walls.end(), order);
    QVector<HandConnection> connections;
    const BeatObject *previousHand[2] = {nullptr, nullptr};
    for (const auto &note : notes) {
        if (note.color < 0 || note.color > 1) continue;
        const auto *prior = previousHand[note.color];
        if (prior) {
            const double dt = request.timeMap.beatToSeconds(note.beat)-request.timeMap.beatToSeconds(prior->beat);
            if (dt < request.profile.minSameHandGapSeconds-epsilon)
                addError(errors, QStringLiteral("第 %1 拍%2手间隔 %3 秒，小于难度限制。")
                         .arg(note.beat).arg(note.color == 0 ? QStringLiteral("左") : QStringLiteral("右")).arg(dt, 0, 'f', 3));
            const QPointF exit = centre(*prior)+cutVector(prior->direction)*0.35;
            const QPointF entry = centre(note)-cutVector(note.direction)*0.35;
            if (length(entry-exit) > qMax(0.0, dt)*request.profile.maxConnectionSpeed+0.15)
                addError(errors, QStringLiteral("第 %1 拍同手入出刀连接距离过远，需要回位。 ").arg(note.beat).trimmed());
            // A short gap implies a continuous exit-to-entry movement. Longer
            // rests permit returning the hand to a comfortable neutral position.
            if (dt > epsilon && dt <= continuousMovementSeconds+epsilon)
                connections.append({request.timeMap.beatToSeconds(prior->beat),
                                    request.timeMap.beatToSeconds(note.beat), exit, entry});
        }
        previousHand[note.color] = &note;
    }
    for (int i = 1; i < bombs.size(); ++i)
        if (request.timeMap.beatToSeconds(bombs[i].beat)-request.timeMap.beatToSeconds(bombs[i-1].beat) < 5.0-epsilon)
            addError(errors, QStringLiteral("炸弹之间至少需要 5 秒。"));
    std::sort(connections.begin(), connections.end(), [](const HandConnection &a, const HandConnection &b) {
        return a.startSeconds < b.startSeconds;
    });
    for (const auto &bomb : bombs) {
        const double seconds = request.timeMap.beatToSeconds(bomb.beat);
        const double firstSeconds = seconds-bombWindowSeconds, lastSeconds = seconds+bombWindowSeconds;
        const double firstBeat = request.timeMap.secondsToBeat(firstSeconds);
        const double lastBeat = request.timeMap.secondsToBeat(lastSeconds);
        auto note = std::lower_bound(notes.constBegin(), notes.constEnd(), firstBeat,
                                    [](const BeatObject &candidate, double beat) { return candidate.beat < beat; });
        for (; note != notes.constEnd() && note->beat <= lastBeat; ++note)
            if (pointToCut(centre(bomb), *note) < 0.6) addError(errors, QStringLiteral("炸弹位于附近击打的挥刀路径。"));
        auto connection = std::lower_bound(connections.constBegin(), connections.constEnd(),
                                          firstSeconds-continuousMovementSeconds-epsilon,
                                          [](const HandConnection &candidate, double time) { return candidate.startSeconds < time; });
        for (; connection != connections.constEnd() && connection->startSeconds <= lastSeconds; ++connection) {
            const double from = qMax(firstSeconds, connection->startSeconds);
            const double to = qMin(lastSeconds, connection->endSeconds);
            if (to < from-epsilon) continue;
            const double dt = connection->endSeconds-connection->startSeconds;
            const QPointF delta = connection->entry-connection->exit;
            const QPointF start = connection->exit+delta*((from-connection->startSeconds)/dt);
            const QPointF end = connection->exit+delta*((to-connection->startSeconds)/dt);
            if (pointToSegment(centre(bomb), start, end) < 0.6)
                addError(errors, QStringLiteral("炸弹位于同手两次击打之间的移动路径。"));
        }
    }
    for (int i = 0; i < walls.size(); ++i) {
        const auto &wall = walls[i];
        const double start = request.timeMap.beatToSeconds(wall.beat);
        const double end = request.timeMap.beatToSeconds(wall.beat+wall.duration);
        if (i && start-request.timeMap.beatToSeconds(walls[i-1].beat) < 10.0-epsilon)
            addError(errors, QStringLiteral("墙之间至少需要 10 秒。"));
        const double firstBeat = request.timeMap.secondsToBeat(start-0.1);
        const double lastBeat = request.timeMap.secondsToBeat(end+0.1);
        auto note = std::lower_bound(notes.constBegin(), notes.constEnd(), firstBeat,
                                    [](const BeatObject &candidate, double beat) { return candidate.beat < beat; });
        for (; note != notes.constEnd() && note->beat <= lastBeat; ++note) {
            if (note->x == wall.x)
                addError(errors, QStringLiteral("墙遮挡了需要击打的音符。"));
        }
    }
    const auto measured = metrics(objects, request.timeMap, analysis.activeSeconds);
    if (measured.peakNps > request.profile.maxPeakNps+epsilon)
        addError(errors, QStringLiteral("连续两秒峰值密度 %1 NPS 超过难度上限。 ").arg(measured.peakNps).trimmed());
    if (measured.averageNps > request.profile.targetMaxNps+epsilon)
        addError(errors, QStringLiteral("整曲平均击打密度超过难度上限，请减少物件。"));
    return errors->isEmpty();
}

bool BeatmapPlayabilityValidator::parseAndValidate(const QJsonObject &json, const GenerationRequest &request,
                                                  const MusicAnalysis &analysis, int segmentIndex,
                                                  const QString &motifId, const QVector<BeatObject> &previous,
                                                  QVector<BeatObject> *objects, QStringList *errors) {
    QStringList localErrors;
    if (!errors) errors = &localErrors;
    errors->clear();
    if (!objects || segmentIndex < 0 || segmentIndex >= analysis.segments.size()) return false;
    objects->clear();
    const auto &segment = analysis.segments[segmentIndex];
    if (!exactKeys(json, {"schemaVersion", "segmentId", "motifId", "objects"})
            || json.value("schemaVersion").toDouble(-1) != 1
            || json.value("segmentId").toString() != segment.id || json.value("motifId").toString() != motifId
            || !json.value("objects").isArray() || json.value("objects").toArray().size() > 96) {
        addError(errors, QStringLiteral("乐句 JSON 结构、标识或物件数量无效。")); return false;
    }
    const auto rows = json.value("objects").toArray();
    for (int i = 0; i < rows.size(); ++i) {
        if (!rows[i].isObject()) { addError(errors, QStringLiteral("物件必须为 JSON 对象。")); continue; }
        const auto row = rows[i].toObject();
        const QString kind = row.value("kind").toString();
        BeatObject object;
        if (!integer(row, "x", &object.x) || !integer(row, "y", &object.y)) {
            addError(errors, QStringLiteral("物件格位必须为整数。")); continue;
        }
        if (kind == "wall") {
            if (!exactKeys(row, {"kind", "startAnchor", "endAnchor", "x", "y", "width", "height"})
                    || !integer(row, "width", &object.width) || !integer(row, "height", &object.height)) {
                addError(errors, QStringLiteral("墙字段无效或含未知字段。")); continue;
            }
            const auto *start = analysis.anchor(row.value("startAnchor").toString());
            const auto *end = analysis.anchor(row.value("endAnchor").toString());
            if (!start || !end || start->kind != MusicAnchorKind::Boundary || end->kind != MusicAnchorKind::Boundary
                    || start->beat < segment.startBeat-epsilon || start->beat >= segment.endBeat-epsilon
                    || end->beat > segment.endBeat+epsilon || end->beat <= start->beat) {
                addError(errors, QStringLiteral("墙起止必须引用当前乐句内有效边界锚点。")); continue;
            }
            object.kind = ObjectKind::Wall; object.beat = start->beat; object.duration = end->beat-start->beat;
        } else {
            const auto *anchor = analysis.anchor(row.value("anchorId").toString());
            if (!anchor || anchor->beat < segment.startBeat-epsilon || anchor->beat >= segment.endBeat-epsilon) {
                addError(errors, QStringLiteral("物件引用了未知或当前乐句之外的锚点。")); continue;
            }
            object.beat = anchor->beat;
            if (kind == "bomb") {
                if (anchor->kind != MusicAnchorKind::Rest || !exactKeys(row, {"kind", "anchorId", "x", "y"})) {
                    addError(errors, QStringLiteral("炸弹必须引用休息锚点且只含规定字段。")); continue;
                }
                object.kind = ObjectKind::Bomb;
            } else if (kind == "directional" || kind == "dot") {
                const QString hand = row.value("hand").toString();
                if (anchor->kind != MusicAnchorKind::Hit || (hand != "left" && hand != "right")
                        || !exactKeys(row, kind == "dot" ? QStringList{"kind", "anchorId", "hand", "x", "y"}
                                                          : QStringList{"kind", "anchorId", "hand", "x", "y", "direction"})) {
                    addError(errors, QStringLiteral("音符必须引用起音锚点，并指定有效左右手及规定字段。")); continue;
                }
                object.kind = ObjectKind::Note; object.color = hand == "left" ? 0 : 1;
                object.direction = 8;
                if (kind == "directional" && (!integer(row, "direction", &object.direction)
                        || object.direction < 0 || object.direction > 7)) {
                    addError(errors, QStringLiteral("方向块方向必须为 0–7。")); continue;
                }
            } else { addError(errors, QStringLiteral("未知生成物件类别。")); continue; }
        }
        objects->append(object);
    }
    if (errors && !errors->isEmpty()) { objects->clear(); return false; }
    QVector<BeatObject> combined = previous;
    combined += *objects;
    if (!validateObjects(combined, request, analysis, errors)) { objects->clear(); return false; }
    std::stable_sort(objects->begin(), objects->end(), [](const BeatObject &a, const BeatObject &b) { return a.beat < b.beat; });
    return true;
}

QJsonObject BeatmapPlayabilityValidator::handContext(const QVector<BeatObject> &objects,
                                                    const TimeMap &timeMap, double segmentStartBeat) {
    QJsonObject result;
    QJsonArray tail, activeWalls;
    const BeatObject *last[2] = {nullptr, nullptr};
    for (const auto &object : objects) {
        if (object.kind == ObjectKind::Note && object.color >= 0 && object.color <= 1
                && (!last[object.color] || object.beat > last[object.color]->beat)) last[object.color] = &object;
        if (object.beat >= segmentStartBeat-2.0)
            tail.append(QJsonObject{{"beat", object.beat}, {"kind", static_cast<int>(object.kind)},
                                   {"x", object.x}, {"y", object.y}, {"hand", object.color}, {"direction", object.direction}});
        if (object.kind == ObjectKind::Wall && object.beat+object.duration > segmentStartBeat)
            activeWalls.append(QJsonObject{{"x", object.x}, {"endBeat", object.beat+object.duration}});
    }
    for (int hand = 0; hand < 2; ++hand) {
        QJsonObject state{{"hasPrevious", last[hand] != nullptr}};
        if (last[hand]) {
            const auto &note = *last[hand];
            const QPointF exit = centre(note)+cutVector(note.direction)*0.35;
            state.insert("beat", note.beat); state.insert("seconds", timeMap.beatToSeconds(note.beat));
            state.insert("x", note.x); state.insert("y", note.y); state.insert("direction", note.direction);
            state.insert("exitX", exit.x()); state.insert("exitY", exit.y());
            state.insert("restSeconds", timeMap.beatToSeconds(segmentStartBeat)-timeMap.beatToSeconds(note.beat));
        }
        result.insert(hand == 0 ? "left" : "right", state);
    }
    result.insert("tail", tail); result.insert("activeWalls", activeWalls);
    return result;
}

QJsonArray BeatmapPlayabilityValidator::motifReference(const QVector<BeatObject> &objects, double startBeat) {
    QJsonArray result;
    for (const auto &object : orderedNotes(objects)) {
        // Hazards have independent spacing/safety requirements. The recurring
        // two-hand movement theme describes hits, not a repeated obstacle quota.
        if (object.kind != ObjectKind::Note) continue;
        result.append(QJsonObject{{"relativeBeat", object.beat-startBeat}, {"kind", static_cast<int>(object.kind)},
                      {"hand", object.color}, {"x", object.x}, {"y", object.y}, {"direction", object.direction},
                      {"duration", object.kind == ObjectKind::Wall ? object.duration : 0.0}});
    }
    return result;
}

QJsonObject MotifComparison::feedback() const {
    return {{"comparable", comparable}, {"referenceNotes", referenceNotes}, {"currentNotes", currentNotes},
            {"matchedActions", matchedActions}, {"actionDifference", actionDifference},
            {"rhythmCoverage", rhythmCoverage}, {"positionDifference", positionDifference},
            {"countDifference", countDifference}, {"differences", differences}};
}

MotifComparison BeatmapPlayabilityValidator::compareMotifs(const QVector<BeatObject> &reference, double referenceStart,
                                                           const QVector<BeatObject> &objects, double startBeat) {
    const auto prior = orderedNotes(reference), current = orderedNotes(objects);
    MotifComparison result;
    result.referenceNotes = prior.size(); result.currentNotes = current.size();
    result.countDifference = qMax(prior.size(), current.size())
        ? std::abs(prior.size()-current.size())/double(qMax(prior.size(), current.size())) : 0;
    if (prior.isEmpty() || current.isEmpty()) return result;
    result.comparable = true;
    // Theme is the ordered hand/cut sequence. Timing and safe grid movement
    // are musical adaptations, measured separately from the action sequence.
    auto sameAction = [](const BeatObject &a, const BeatObject &b) {
        return a.color == b.color && a.direction == b.direction;
    };
    QVector<QVector<int>> lcs(prior.size()+1, QVector<int>(current.size()+1));
    for (int i=prior.size()-1; i>=0; --i)
        for (int j=current.size()-1; j>=0; --j)
            lcs[i][j] = sameAction(prior[i], current[j]) ? 1+lcs[i+1][j+1] : qMax(lcs[i+1][j], lcs[i][j+1]);
    result.matchedActions = lcs[0][0];
    result.actionDifference = 1-result.matchedActions/double(qMin(prior.size(), current.size()));
    int moved=0, i=0, j=0;
    while (i<prior.size() || j<current.size()) {
        if (i<prior.size() && j<current.size() && sameAction(prior[i], current[j])) {
            if (prior[i].x!=current[j].x || prior[i].y!=current[j].y) {
                ++moved;
                result.differences.append(QJsonObject{{"kind","position"},{"referenceIndex",i},{"currentIndex",j}});
            }
            ++i; ++j;
        } else if (i<prior.size() && (j==current.size() || lcs[i+1][j]>=lcs[i][j+1])) {
            result.differences.append(QJsonObject{{"kind","referenceAction"},{"referenceIndex",i++}});
        } else result.differences.append(QJsonObject{{"kind","currentAction"},{"currentIndex",j++}});
    }
    result.positionDifference = result.matchedActions ? moved/double(result.matchedActions) : 0;
    QSet<qint64> beats;
    for (const auto &note : current) beats.insert(qRound64((note.beat-startBeat)*1000));
    int covered=0;
    for (const auto &note : prior) if (beats.contains(qRound64((note.beat-referenceStart)*1000))) ++covered;
    result.rhythmCoverage = covered/double(prior.size());
    return result;
}

} // namespace lmsc
