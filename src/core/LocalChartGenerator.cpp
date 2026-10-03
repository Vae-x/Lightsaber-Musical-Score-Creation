#include "LocalChartGenerator.h"
#include "BeatmapPlayabilityValidator.h"

#include <QHash>
#include <QPointF>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace lmsc {
namespace {
constexpr double epsilon = 1e-7;
constexpr int beamWidth = 6;
constexpr int lookahead = 4;
constexpr int maximumAnchors = 250000;
constexpr int maximumObjects = 40000;

QPointF cutVector(int direction) {
    const double diagonal = std::sqrt(0.5);
    const QPointF vectors[] = {{0,1}, {0,-1}, {-1,0}, {1,0}, {-diagonal,diagonal},
                               {diagonal,diagonal}, {-diagonal,-diagonal}, {diagonal,-diagonal}, {0,0}};
    return direction >= 0 && direction <= 8 ? vectors[direction] : QPointF{};
}
QPointF centre(const BeatObject &object) { return {double(object.x), double(object.y)}; }
double length(const QPointF &point) { return std::hypot(point.x(), point.y()); }
double dot(const QPointF &a, const QPointF &b) { return a.x()*b.x()+a.y()*b.y(); }
int opposite(int direction) {
    constexpr int directions[] = {1,0,3,2,7,6,5,4,8};
    return direction >= 0 && direction <= 8 ? directions[direction] : 1;
}
double pointToSegment(const QPointF &point, const QPointF &start, const QPointF &end) {
    const QPointF delta = end-start;
    const double square = dot(delta,delta);
    const double t = square < epsilon ? 0.0 : qBound(0.0,dot(point-start,delta)/square,1.0);
    return length(point-start-delta*t);
}

struct Hit {
    const MusicAnchor *anchor = nullptr;
    double seconds = 0.0, score = 0.0;
};
struct Action { int hand = 0, direction = 1, x = 1, y = 1; };
struct Theme { int family = 0; QVector<Action> actions; };
struct Hand { bool present = false; BeatObject note; double seconds = 0.0; };
struct Decision { bool place = false; BeatObject note; };
struct SearchState {
    std::array<Hand,2> hands;
    std::array<int,2> counts{{0,0}};
    int lastHand = -1;
    double lastSeconds = -std::numeric_limits<double>::infinity(), cost = 0.0;
    Decision first;
};
struct Connection { double start = 0.0, end = 0.0; QPointF exit, entry; };

Action expectedAction(const Theme &theme, const Hit &hit, int index, int count,
                      const GenerationRequest &request) {
    if (!theme.actions.isEmpty()) {
        const int reference = qMin(theme.actions.size()-1,
                                  int(qint64(index)*theme.actions.size()/qMax(1,count)));
        return theme.actions[reference];
    }
    // Four bounded movement themes. Each hand reverses its previous stroke;
    // changes of rhythm are handled by the search rather than random positions.
    static const int cuts[4][4] = {{1,1,0,0}, {6,7,5,4}, {2,3,3,2}, {1,7,0,4}};
    Action result;
    result.hand = index%2;
    result.direction = cuts[theme.family][index%4];
    result.x = result.hand == 0 ? 1 : 2;
    if (request.profile.rank >= 5 && (index%16 >= 4 && index%16 < 8)
            && hit.anchor->strength > .65) result.y = 2;
    if (!request.allowedTypes.testFlag(DirectionalType)
            || (request.allowedTypes.testFlag(DotType)
                && (index%8 == 6 || hit.anchor->confidence < .45))) result.direction = 8;
    return result;
}

QVector<BeatObject> actionCandidates(const SearchState &state, const Hit &hit,
                                    const Action &preferred, const GenerationRequest &request) {
    QVector<BeatObject> result;
    result.reserve(32);
    for (int order=0; order<2; ++order) {
        const int hand = order == 0 ? preferred.hand : 1-preferred.hand;
        QVector<int> directions;
        auto addDirection = [&](int direction) {
            if ((direction == 8 && request.allowedTypes.testFlag(DotType))
                    || (direction < 8 && request.allowedTypes.testFlag(DirectionalType)))
                if (!directions.contains(direction)) directions.append(direction);
        };
        addDirection(preferred.direction);
        if (state.hands[hand].present) addDirection(opposite(state.hands[hand].note.direction));
        addDirection(1); addDirection(0);
        if (preferred.direction == 8) addDirection(8);
        const int home = hand == 0 ? 1 : 2;
        const int preferredX = hand == preferred.hand ? preferred.x : 3-preferred.x;
        QVector<QPoint> positions{{preferredX,preferred.y}, {home,1}};
        if (state.hands[hand].present) positions.append({state.hands[hand].note.x,state.hands[hand].note.y});
        QSet<int> seenPositions;
        for (const auto &position : positions) {
            if (position.x() < hand*2 || position.x() > hand*2+1 || position.y() < 0 || position.y() > 2) continue;
            const int key = position.x()*3+position.y();
            if (seenPositions.contains(key)) continue;
            seenPositions.insert(key);
            for (int direction : directions) {
                BeatObject object;
                object.kind = ObjectKind::Note; object.beat = hit.anchor->beat;
                object.color = hand; object.direction = direction;
                object.x = position.x(); object.y = position.y();
                result.append(object);
            }
        }
    }
    return result;
}

bool advanceState(const SearchState &source, const BeatObject &note, double seconds,
                  const Action &preferred, const DifficultyProfile &profile, SearchState *target) {
    const Hand &previous = source.hands[note.color];
    double cost = 0.0;
    if (previous.present) {
        const double dt = seconds-previous.seconds;
        if (dt < profile.minSameHandGapSeconds-epsilon) return false;
        const QPointF exit = centre(previous.note)+cutVector(previous.note.direction)*.35;
        const QPointF entry = centre(note)-cutVector(note.direction)*.35;
        const double distance = length(entry-exit);
        const double allowance = dt*profile.maxConnectionSpeed+.15;
        if (distance > allowance+epsilon) return false;
        cost += 1.2*distance/qMax(.15,allowance);
        if (previous.note.direction != 8 && note.direction != 8) {
            const double agreement = dot(cutVector(previous.note.direction),cutVector(note.direction));
            // Fast sequences need a reversal rather than an invisible reset.
            if (dt <= 1.0+epsilon && agreement > .1) return false;
            cost += qMax(0.0,agreement+.4)*(dt <= 1.5 ? 2.5 : .6);
        }
    }
    cost += note.color == preferred.hand ? 0.0 : 1.2;
    cost += note.direction == preferred.direction ? 0.0 : 1.0;
    cost += .18*(std::abs(note.x-(note.color == preferred.hand ? preferred.x : 3-preferred.x))
                  +std::abs(note.y-preferred.y));
    if (source.lastHand == note.color && seconds-source.lastSeconds < .6) cost += .6;
    *target = source;
    target->hands[note.color] = {true,note,seconds};
    ++target->counts[note.color];
    cost += .03*std::abs(target->counts[0]-target->counts[1]);
    target->lastHand = note.color; target->lastSeconds = seconds; target->cost += cost;
    return true;
}

Decision searchNext(const QVector<Hit> &hits, const QVector<Action> &preferred, int start,
                    const SearchState &initial, const GenerationRequest &request,
                    const std::function<bool()> &cancelled, bool *stopped) {
    QVector<SearchState> beam{initial};
    for (int step=start; step<qMin(hits.size(),start+lookahead); ++step) {
        if (cancelled && cancelled()) { *stopped = true; return {}; }
        QVector<SearchState> next;
        next.reserve(beamWidth*32);
        for (const auto &state : beam) {
            for (const auto &note : actionCandidates(state,hits[step],preferred[step],request)) {
                SearchState candidate;
                if (!advanceState(state,note,hits[step].seconds,preferred[step],request.profile,&candidate)) continue;
                if (step == start) candidate.first = {true,note};
                next.append(candidate);
            }
            SearchState skipped = state;
            skipped.cost += 5.0+5.0*hits[step].score;
            if (step == start) skipped.first = {};
            next.append(skipped);
        }
        std::stable_sort(next.begin(),next.end(),[](const SearchState &a,const SearchState &b) {
            return a.cost < b.cost;
        });
        if (next.size() > beamWidth) next.resize(beamWidth);
        beam = std::move(next);
    }
    return beam.first().first;
}

bool peakFits(const QVector<double> &times, double proposed, int maximumWindowCount) {
    QVector<double> test = times;
    test.insert(std::lower_bound(test.begin(),test.end(),proposed),proposed);
    int left=0;
    for (int right=0; right<test.size(); ++right) {
        while (left<right && test[right]-test[left] >= 2.0-epsilon) ++left;
        if (right-left+1 > maximumWindowCount) return false;
    }
    return true;
}

bool validAnchor(const MusicAnchor &anchor, const GenerationRequest &request,
                 const MusicAnalysis &analysis) {
    const double seconds = request.timeMap.beatToSeconds(anchor.beat);
    return std::isfinite(anchor.beat) && anchor.beat >= 0 && std::isfinite(anchor.seconds)
        && std::isfinite(anchor.strength) && std::isfinite(anchor.confidence)
        && std::isfinite(seconds) && seconds >= 0
        && (anchor.kind == MusicAnchorKind::Boundary ? seconds <= analysis.durationSeconds+epsilon
                                                    : seconds < analysis.durationSeconds)
        && std::abs(seconds-anchor.seconds) <= .002;
}

bool bombSafe(const BeatObject &bomb, double seconds, const QVector<BeatObject> &notes,
              const QVector<double> &noteTimes, const QVector<Connection> &connections) {
    const double start = seconds-.2, end = seconds+.2;
    auto note = std::lower_bound(noteTimes.begin(),noteTimes.end(),start-epsilon);
    for (int i=int(note-noteTimes.begin()); i<notes.size() && noteTimes[i] <= end+epsilon; ++i) {
        const QPointF vector = cutVector(notes[i].direction)*.35;
        if (pointToSegment(centre(bomb),centre(notes[i])-vector,centre(notes[i])+vector) < .6) return false;
    }
    auto connection = std::lower_bound(connections.begin(),connections.end(),start-1.0-epsilon,
                                      [](const Connection &item,double value) { return item.start<value; });
    for (; connection != connections.end() && connection->start <= end+epsilon; ++connection) {
        const double from=qMax(start,connection->start), to=qMin(end,connection->end);
        if (to < from-epsilon) continue;
        const QPointF delta=connection->entry-connection->exit;
        const double dt=connection->end-connection->start;
        const QPointF a=connection->exit+delta*((from-connection->start)/dt);
        const QPointF b=connection->exit+delta*((to-connection->start)/dt);
        if (pointToSegment(centre(bomb),a,b) < .6) return false;
    }
    return true;
}

bool wallSafe(int x,double start,double end,const QVector<BeatObject> &notes,
              const QVector<double> &noteTimes) {
    auto note=std::lower_bound(noteTimes.begin(),noteTimes.end(),start-.1-epsilon);
    for (int i=int(note-noteTimes.begin()); i<notes.size() && noteTimes[i]<=end+.1+epsilon; ++i)
        if (notes[i].x==x) return false;
    return true;
}
} // namespace

bool LocalChartGenerator::generate(const GenerationRequest &request,const MusicAnalysis &analysis,
                                   GenerationDraft *draft,QString *error,
                                   const std::function<bool()> &cancelled,
                                   const std::function<void(int)> &progress) {
    if (error) error->clear();
    if (!draft) { if (error) *error=QStringLiteral("没有提供本地制谱草稿容器。"); return false; }
    *draft={};
    auto fail=[&](const QString &message) { if (error) *error=message; return false; };
    auto stopped=[&] { return cancelled && cancelled(); };
    if (stopped()) return fail(QStringLiteral("已取消本地制谱。"));
    const auto &profile=request.profile;
    const int types=int(request.allowedTypes);
    if ((types & ~15) || (!request.analysisOnly && !types)
            || !QStringList{"Easy","Normal","Hard","Expert","ExpertPlus"}.contains(profile.name)
            || !std::isfinite(analysis.durationSeconds) || analysis.durationSeconds<=0 || analysis.durationSeconds>3600
            || !std::isfinite(analysis.activeSeconds) || analysis.activeSeconds<=0
            || analysis.activeSeconds>analysis.durationSeconds+epsilon || analysis.segments.isEmpty()
            || analysis.anchors.size()>maximumAnchors || analysis.segments.size()>10000
            || !std::isfinite(profile.targetMinNps) || profile.targetMinNps<0
            || !std::isfinite(profile.targetMaxNps) || profile.targetMaxNps<=0 || profile.targetMaxNps>10
            || profile.targetMinNps>profile.targetMaxNps || !std::isfinite(profile.maxPeakNps)
            || profile.maxPeakNps<=0 || profile.maxPeakNps>20
            || !std::isfinite(profile.minSameHandGapSeconds) || profile.minSameHandGapSeconds<=0
            || !std::isfinite(profile.maxConnectionSpeed) || profile.maxConnectionSpeed<=0
            || profile.subdivision<1 || profile.subdivision>4)
        return fail(QStringLiteral("本地制谱的音乐分析、难度或物件选项无效。"));

    int hitCandidates=0;
    double maximumEnergy=0.0;
    for (int i=0; i<analysis.anchors.size(); ++i) {
        if ((i & 255)==0 && stopped()) return fail(QStringLiteral("已取消本地制谱。"));
        const auto &anchor=analysis.anchors[i];
        if (!validAnchor(anchor,request,analysis)) return fail(QStringLiteral("音乐锚点与当前拍线不一致，请重新分析。"));
        if (anchor.kind==MusicAnchorKind::Hit) ++hitCandidates;
    }
    double previousEnd=-1.0;
    for (const auto &segment : analysis.segments) {
        if (!std::isfinite(segment.startBeat) || !std::isfinite(segment.endBeat)
                || segment.startBeat<0 || segment.endBeat<=segment.startBeat
                || !std::isfinite(segment.energy) || segment.energy<0
                || !std::isfinite(segment.activeSeconds) || segment.activeSeconds<0
                || segment.startBeat<previousEnd-epsilon || segment.anchors.size()>128)
            return fail(QStringLiteral("音乐段落范围无效，请重新分析。"));
        for (int index : segment.anchors)
            if (index<0 || index>=analysis.anchors.size()) return fail(QStringLiteral("音乐段落引用了无效锚点。"));
        maximumEnergy=qMax(maximumEnergy,segment.energy); previousEnd=segment.endBeat;
    }

    GenerationDraft result;
    result.source=request; result.warnings=analysis.warnings;
    if (request.analysisOnly) {
        result.summary=QStringLiteral("本地音乐分析：时长 %1 秒，%2 个段落，%3 个起音候选；未调用大语言模型。BPM 与偏移仍需试听确认。")
            .arg(analysis.durationSeconds,0,'f',1).arg(analysis.segments.size()).arg(hitCandidates);
        *draft=std::move(result); if (progress) progress(100); return true;
    }
    if (progress) progress(0);
    QVector<BeatObject> notes;
    QVector<double> noteTimes;
    const int maximumHits=qMin(maximumObjects,int(std::floor(analysis.activeSeconds*profile.targetMaxNps+epsilon)));
    const int maximumWindowCount=int(std::floor(profile.maxPeakNps*2+epsilon));
    SearchState state;
    QHash<QString,Theme> themes;
    QSet<qint64> usedHitBeats;
    int skippedForMovement=0, repeatedAdaptations=0;
    if (request.allowedTypes.testFlag(DirectionalType) || request.allowedTypes.testFlag(DotType)) {
        for (int segmentIndex=0; segmentIndex<analysis.segments.size(); ++segmentIndex) {
            if (stopped()) return fail(QStringLiteral("已取消本地制谱。"));
            const auto &segment=analysis.segments[segmentIndex];
            if (segment.activeSeconds<=0 || segment.energy<=1e-6) continue;
            QVector<Hit> ranked;
            QSet<qint64> seen;
            for (int index : segment.anchors) {
                const auto &anchor=analysis.anchors[index];
                const qint64 tick=qRound64(anchor.beat*1000000);
                if (anchor.kind!=MusicAnchorKind::Hit || anchor.beat<segment.startBeat-epsilon
                        || anchor.beat>=segment.endBeat-epsilon || anchor.confidence<.15
                        || anchor.strength<.025 || seen.contains(tick) || usedHitBeats.contains(tick)) continue;
                seen.insert(tick);
                const double fraction=std::abs(anchor.beat-std::round(anchor.beat));
                const double metrical=fraction<epsilon ? 1.0 : std::abs(fraction-.5)<epsilon ? .65 : .35;
                ranked.append({&anchor,anchor.seconds,.6*qBound(0.0,anchor.strength,1.0)
                    +.25*qBound(0.0,anchor.confidence,1.0)+.15*metrical});
            }
            if (ranked.size()>128) return fail(QStringLiteral("单段音乐起音过多，请重新分析拍格。"));
            std::stable_sort(ranked.begin(),ranked.end(),[](const Hit &a,const Hit &b) {
                return a.score!=b.score ? a.score>b.score : a.seconds<b.seconds;
            });
            const double energy=qBound(0.0,segment.energy/qMax(1e-12,maximumEnergy),1.0);
            const double target=profile.targetMinNps+(profile.targetMaxNps-profile.targetMinNps)*(.15+.75*std::sqrt(energy));
            const int budget=qMin(maximumHits-notes.size(),qMax(1,int(std::floor(target*segment.activeSeconds+.5))));
            QVector<double> recentTimes;
            const double segmentStart=request.timeMap.beatToSeconds(segment.startBeat);
            auto prior=std::lower_bound(noteTimes.begin(),noteTimes.end(),segmentStart-2.0-epsilon);
            for (; prior!=noteTimes.end(); ++prior) recentTimes.append(*prior);
            QVector<Hit> selected;
            const double minimumGap=profile.minSameHandGapSeconds*.5;
            for (const auto &hit : ranked) {
                if (selected.size()>=budget) break;
                bool close=false;
                for (const auto &chosen : selected)
                    if (std::abs(chosen.seconds-hit.seconds)<minimumGap-epsilon) { close=true; break; }
                if (close || !peakFits(recentTimes,hit.seconds,maximumWindowCount)) continue;
                selected.append(hit);
                recentTimes.insert(std::lower_bound(recentTimes.begin(),recentTimes.end(),hit.seconds),hit.seconds);
            }
            std::sort(selected.begin(),selected.end(),[](const Hit &a,const Hit &b) { return a.seconds<b.seconds; });
            if (selected.isEmpty()) continue;
            const QString key=segment.repeatGroup.isEmpty() ? QStringLiteral("segment:%1").arg(segmentIndex) : segment.repeatGroup;
            if (!themes.contains(key)) {
                Theme theme;
                theme.family=profile.rank<=3 ? 0 : energy>.8 ? 1 : energy>.5 ? 3 : 2;
                themes.insert(key,theme);
            }
            const Theme theme=themes.value(key);
            QVector<Action> preferred;
            preferred.reserve(selected.size());
            for (int i=0; i<selected.size(); ++i) preferred.append(expectedAction(theme,selected[i],i,selected.size(),request));
            QVector<Action> realised;
            for (int i=0; i<selected.size(); ++i) {
                bool searchStopped=false;
                const Decision decision=searchNext(selected,preferred,i,state,request,cancelled,&searchStopped);
                if (searchStopped) return fail(QStringLiteral("已取消本地制谱。"));
                if (!decision.place) { ++skippedForMovement; continue; }
                SearchState next;
                if (!advanceState(state,decision.note,selected[i].seconds,preferred[i],profile,&next))
                    return fail(QStringLiteral("本地动作衔接状态失效，未生成草稿。"));
                next.cost=0; state=next;
                notes.append(decision.note); noteTimes.append(selected[i].seconds);
                usedHitBeats.insert(qRound64(decision.note.beat*1000000));
                realised.append({decision.note.color,decision.note.direction,decision.note.x,decision.note.y});
                if (!theme.actions.isEmpty() && (decision.note.color!=preferred[i].hand || decision.note.direction!=preferred[i].direction))
                    ++repeatedAdaptations;
            }
            if (themes[key].actions.isEmpty() && !realised.isEmpty()) themes[key].actions=realised;
            if (progress) progress((segmentIndex+1)*80/analysis.segments.size());
        }
    }
    if (stopped()) return fail(QStringLiteral("已取消本地制谱。"));
    result.objects=notes;

    QVector<Connection> connections;
    std::array<Hand,2> hands;
    for (int i=0; i<notes.size(); ++i) {
        const auto &note=notes[i];
        const Hand &previous=hands[note.color];
        const double dt=noteTimes[i]-previous.seconds;
        if (previous.present && dt>epsilon && dt<=1.0+epsilon)
            connections.append({previous.seconds,noteTimes[i],
                centre(previous.note)+cutVector(previous.note.direction)*.35,
                centre(note)-cutVector(note.direction)*.35});
        hands[note.color]={true,note,noteTimes[i]};
    }
    std::sort(connections.begin(),connections.end(),[](const Connection &a,const Connection &b) { return a.start<b.start; });
    QVector<const MusicAnchor *> rests,boundaries,activity;
    for (const auto &anchor : analysis.anchors) {
        if (anchor.kind==MusicAnchorKind::Rest) rests.append(&anchor);
        if (anchor.kind==MusicAnchorKind::Boundary) boundaries.append(&anchor);
        else activity.append(&anchor);
    }
    auto order=[](const MusicAnchor *a,const MusicAnchor *b) { return a->seconds<b->seconds; };
    std::sort(rests.begin(),rests.end(),order); std::sort(boundaries.begin(),boundaries.end(),order);
    std::sort(activity.begin(),activity.end(),order);
    if (request.allowedTypes.testFlag(BombType)) {
        double lastBomb=-std::numeric_limits<double>::infinity();
        int bombCount=0;
        for (int i=0; i<rests.size(); ++i) {
            if ((i & 31)==0 && stopped()) return fail(QStringLiteral("已取消本地制谱。"));
            const auto &anchor=*rests[i];
            if (anchor.seconds-lastBomb<5.0+epsilon) continue;
            bool placed=false;
            for (int side=0; side<2 && !placed; ++side) for (int row : {2,0}) {
                BeatObject bomb; bomb.kind=ObjectKind::Bomb; bomb.beat=anchor.beat;
                bomb.x=(bombCount+side)%2==0 ? 0 : 3; bomb.y=row;
                if (!bombSafe(bomb,anchor.seconds,notes,noteTimes,connections)) continue;
                result.objects.append(bomb); lastBomb=anchor.seconds; ++bombCount; placed=true; break;
            }
        }
    }
    if (progress) progress(88);

    // Conservative activity spans prevent side walls extending into a silent
    // gap. Dense attacks can bridge their short musical gaps without requiring
    // the analyser to label every subdivision as a Rest anchor.
    QVector<QPair<double,double>> spans;
    for (int i=0; i<activity.size(); ++i) {
        if ((i & 255)==0 && stopped()) return fail(QStringLiteral("已取消本地制谱。"));
        const auto &anchor=*activity[i];
        const double gap=qBound(.4,1.1*60.0/request.timeMap.bpmAtBeat(anchor.beat),2.0);
        if (!spans.isEmpty() && anchor.seconds-spans.last().second<=gap+.08) spans.last().second=anchor.seconds+.08;
        else spans.append(qMakePair(qMax(0.0,anchor.seconds-.08),qMin(analysis.durationSeconds,anchor.seconds+.08)));
    }
    if (request.allowedTypes.testFlag(WallType)) {
        double lastWall=-std::numeric_limits<double>::infinity();
        int wallCount=0,spanIndex=0;
        for (int i=0; i<boundaries.size(); ++i) {
            if ((i & 31)==0 && stopped()) return fail(QStringLiteral("已取消本地制谱。"));
            const auto &start=*boundaries[i];
            if (start.seconds-lastWall<10.0+epsilon) continue;
            while (spanIndex<spans.size() && spans[spanIndex].second<start.seconds) ++spanIndex;
            if (spanIndex>=spans.size()) break;
            if (start.seconds<spans[spanIndex].first) continue;
            // A bridged interval describes musical continuity, not evidence
            // that every boundary is audible. Start only near an actual
            // attack or energetic Rest; bare boundaries include silent gaps.
            const auto evidence=std::lower_bound(activity.constBegin(),activity.constEnd(),start.seconds-.08,
                [](const MusicAnchor *anchor,double seconds) { return anchor->seconds<seconds; });
            if (evidence==activity.constEnd() || (*evidence)->seconds>start.seconds+.08) continue;
            const MusicAnchor *bestEnd=nullptr;
            double bestDistance=std::numeric_limits<double>::infinity();
            for (int j=i+1; j<boundaries.size() && boundaries[j]->seconds<=start.seconds+2.0+epsilon; ++j) {
                const auto *end=boundaries[j];
                const double duration=end->seconds-start.seconds;
                if (duration<.5+epsilon || end->seconds>spans[spanIndex].second+epsilon) continue;
                if (std::abs(duration-1.0)<bestDistance) { bestDistance=std::abs(duration-1.0); bestEnd=end; }
            }
            if (!bestEnd) continue;
            for (int side=0; side<2; ++side) {
                const int x=(wallCount+side)%2==0 ? 0 : 3;
                if (!wallSafe(x,start.seconds,bestEnd->seconds,notes,noteTimes)) continue;
                BeatObject wall; wall.kind=ObjectKind::Wall; wall.beat=start.beat;
                wall.duration=bestEnd->beat-start.beat; wall.x=x; wall.y=0; wall.width=1; wall.height=5;
                result.objects.append(wall); lastWall=start.seconds; ++wallCount; break;
            }
        }
    }
    if (stopped()) return fail(QStringLiteral("已取消本地制谱。"));
    if (result.objects.isEmpty()) return fail(QStringLiteral("未找到适合所选物件类型的可靠音乐锚点，未创建空谱。请检查 BPM、偏移或物件选项。"));
    if (result.objects.size()>maximumObjects) return fail(QStringLiteral("本地制谱物件超过安全数量上限。"));
    std::stable_sort(result.objects.begin(),result.objects.end(),[](const BeatObject &a,const BeatObject &b) { return a.beat<b.beat; });
    if (progress) progress(96);
    QStringList validationErrors;
    if (!BeatmapPlayabilityValidator::validateObjects(result.objects,request,analysis,&validationErrors))
        return fail(QStringLiteral("本地草稿未通过整曲动作校验：\n%1").arg(validationErrors.join('\n')));
    if (stopped()) return fail(QStringLiteral("已取消本地制谱。"));
    result.metrics=BeatmapPlayabilityValidator::metrics(result.objects,request.timeMap,analysis.activeSeconds);
    if (skippedForMovement) result.warnings.append(QStringLiteral("为保证同手间隔和挥刀衔接，跳过了 %1 个起音候选。").arg(skippedForMovement));
    if (repeatedAdaptations) result.warnings.append(QStringLiteral("重复段中 %1 个动作根据当前起音和两手衔接作了调整。").arg(repeatedAdaptations));
    if (!notes.isEmpty() && result.metrics.averageNps+epsilon<profile.targetMinNps)
        result.warnings.append(QStringLiteral("可靠起音不足以达到难度的目标密度；已保留真实音乐节奏，未补造起音。"));
    result.warnings.append(QStringLiteral("本地动作校验是桌面近似，新增制谱方式的实际手感仍需头显游玩确认。"));
    result.summary=QStringLiteral("本地快速制谱：根据起音强度、置信度和段落能量选点，用固定动作主题与前瞻搜索安排双手；重复段沿用主题并适配当前节奏。共 %1 个方向块、%2 个无方向块、%3 个炸弹、%4 个侧墙；未调用大语言模型。")
        .arg(result.metrics.directional).arg(result.metrics.dots).arg(result.metrics.bombs).arg(result.metrics.walls);
    *draft=std::move(result);
    if (progress) progress(100);
    return true;
}

} // namespace lmsc
