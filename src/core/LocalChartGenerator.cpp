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
int mirrored(int direction) {
    constexpr int directions[] = {0,1,3,2,5,4,7,6,8};
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
struct Action { int hand = 0, direction = 1, x = 1, y = 1; bool keepAxis = false; };
struct Theme {
    ActionFamily family = ActionFamily::Vertical;
    QHash<qint64, Action> core;
};
struct PhraseTheme {
    ActionFamily family = ActionFamily::Vertical;
    SpaceAmplitude amplitude = SpaceAmplitude::Medium;
    RhythmStrategy rhythm = RhythmStrategy::Balanced;
    ThemeDevelopment development = ThemeDevelopment::Introduce;
    int variant = 0, leadHand = 0;
    double startBeat = 0, endBeat = 0, strokeBeats = 1, accentThreshold = .7;
};
struct Hand { bool present = false; BeatObject note; double seconds = 0.0; };
struct Decision { bool place = false; BeatObject note; };
struct SearchState {
    std::array<Hand,2> hands;
    std::array<int,2> counts{{0,0}};
    int lastHand = -1;
    double lastSeconds = -std::numeric_limits<double>::infinity(), cost = 0.0;
    QVector<Action> recentActions;
    Decision first;
};
struct Connection { double start = 0.0, end = 0.0; QPointF exit, entry; };

quint32 mix(quint32 value) {
    value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15;
    value *= 0x846ca68bu; return value ^ (value >> 16);
}
quint32 musicSeed(const GenerationRequest &request, const MusicAnalysis &analysis) {
    quint32 value = 2166136261u ^ request.arrangementSeed;
    for (QChar ch : analysis.audioFingerprint) value = (value ^ ch.unicode()) * 16777619u;
    return mix(value);
}
bool sameAction(const Action &a, const Action &b) {
    return a.hand == b.hand && a.direction == b.direction && a.x == b.x && a.y == b.y;
}

Action expectedAction(const PhraseTheme &theme, const Hit &hit, const GenerationRequest &request,
                      int previousPreferredHand) {
    Action result;
    const double elapsed = hit.anchor->beat - theme.startBeat;
    const double duration = qMax(.001, theme.endBeat - theme.startBeat);
    const double progress = qBound(0.0, elapsed / duration, .999);
    const int beatCell = qMax(0, int(std::floor(elapsed / qMax(.25, theme.strokeBeats) + 1e-7)));
    // Time/metrical phase, rather than note count, keeps a motif aligned when
    // onset selection differs between two performances of the same section.
    const int stroke = beatCell / 2;
    const bool reverse = (stroke + (theme.variant & 1)) % 2 != 0;
    result.hand = previousPreferredHand < 0 ? theme.leadHand : 1 - previousPreferredHand;
    if (theme.family == ActionFamily::QuestionAnswer) {
        const int group = int(std::floor(elapsed / 2.0 + 1e-7));
        result.hand = (group + theme.leadHand) % 2;
    }
    const bool easy = request.profile.rank == 1;
    const bool wide = !easy && request.profile.rank >= 5 && theme.amplitude == SpaceAmplitude::Wide;
    const bool outer = !easy && theme.amplitude != SpaceAmplitude::Compact;
    int x = 1, y = 1;
    switch (theme.family) {
    case ActionFamily::Vertical:
        result.direction = reverse ? 0 : 1;
        y = wide ? (stroke + theme.variant) % 3 : (stroke + theme.variant) % 2;
        if (outer && progress >= .5) x = 0;
        break;
    case ActionFamily::Diagonal:
        result.direction = reverse ? 5 : 6;
        x = outer && (stroke + theme.variant) % 2 ? 0 : 1;
        y = wide ? (reverse ? 2 : 0) : (reverse ? 1 : 0);
        break;
    case ActionFamily::Horizontal:
        result.direction = reverse ? 3 : 2;
        x = outer ? (reverse ? 1 : 0) : 1;
        y = wide ? theme.variant % 3 : theme.variant % 2;
        break;
    case ActionFamily::Staircase: {
        const int stairs[] = {0, 1, 2, 1};
        result.direction = reverse ? 0 : 1;
        y = wide ? stairs[(stroke + theme.variant) % 4] : (stroke + theme.variant) % 2;
        x = outer && progress > .5 ? 0 : 1;
        break;
    }
    case ActionFamily::Expansion:
        result.direction = reverse ? 5 : 6;
        x = outer && ((progress > .25 && progress < .75) != bool(theme.variant & 1)) ? 0 : 1;
        y = wide ? (progress < .33 ? 0 : progress < .66 ? 1 : 2) : (progress < .5 ? 0 : 1);
        break;
    case ActionFamily::QuestionAnswer:
        result.direction = (beatCell + theme.variant) % 2 ? 5 : 6;
        x = outer && (result.hand == theme.leadHand ? progress < .5 : progress >= .5) ? 0 : 1;
        y = wide ? int(progress * 3) : int(progress * 2);
        break;
    }
    result.x = result.hand == 0 ? x : 3 - x;
    result.y = y;
    if (result.hand == 1) result.direction = mirrored(result.direction);
    // Dot choices are made below from measured accents and phrase endings.
    if (!request.allowedTypes.testFlag(DirectionalType)) result.direction = 8;
    return result;
}

PhraseTheme phraseTheme(const SectionArrangement &section, const MusicPhrase &phrase,
                        const QVector<int> &recentFamilies, quint32 seed, int phraseIndex,
                        const GenerationRequest &request, const Theme *repeatedCore) {
    PhraseTheme theme;
    theme.startBeat = phrase.startBeat; theme.endBeat = phrase.endBeat;
    theme.amplitude = request.profile.rank == 1 ? SpaceAmplitude::Compact
        : request.profile.rank <= 3 && section.amplitude == SpaceAmplitude::Wide ? SpaceAmplitude::Medium : section.amplitude;
    theme.rhythm = section.rhythm; theme.development = section.development;
    theme.variant = int(mix(seed ^ quint32(phraseIndex * 109 + 17)) % 4);
    theme.leadHand = int(mix(seed ^ quint32(phraseIndex * 997 + 29)) & 1);
    std::array<double, 6> score{{phrase.low * .7, phrase.mid * .7 + phrase.syncopation * .4,
        phrase.high * .9, qMax(0.0, phrase.energyTrend) * 2 + .15,
        phrase.energy * .4 + .2, phrase.syncopation * .6 + phrase.restFraction * .4}};
    score[int(section.family)] += 1.0;
    switch (section.development) {
    case ThemeDevelopment::Introduce: score[int(section.family)]+=.2; break;
    case ThemeDevelopment::Repeat: score[int(section.family)]+=.4; break;
    case ThemeDevelopment::Vary:
        score[(int(section.family)+1)%6]+=.35;
        theme.variant=(theme.variant+1)%4;
        break;
    case ThemeDevelopment::Contrast:
        score[int(section.family)]-=.3; score[(int(section.family)+3)%6]+=.6;
        theme.leadHand=1-theme.leadHand;
        break;
    case ThemeDevelopment::Build:
        score[int(ActionFamily::Staircase)]+=.35; score[int(ActionFamily::Expansion)]+=.45;
        break;
    case ThemeDevelopment::Close:
        score[int(ActionFamily::Vertical)]+=.45; score[int(ActionFamily::QuestionAnswer)]+=.25;
        break;
    }
    for (int family = 0; family < 6; ++family) {
        score[family] += (mix(seed ^ quint32(phraseIndex * 131 + family * 977)) % 1000) / 1000.0 * .8;
        for (int age = 0; age < recentFamilies.size(); ++age)
            if (recentFamilies[recentFamilies.size() - 1 - age] == family) score[family] -= 1.5 / (age + 1);
    }
    theme.family = ActionFamily(int(std::max_element(score.begin(), score.end()) - score.begin()));
    if (repeatedCore) theme.family = repeatedCore->family;
    return theme;
}

QVector<BeatObject> actionCandidates(const SearchState &state, const Hit &hit,
                                    const Action &preferred, const GenerationRequest &request) {
    QVector<BeatObject> result;
    result.reserve(64);
    for (int order=0; order<2; ++order) {
        const int hand = order == 0 ? preferred.hand : 1-preferred.hand;
        QVector<int> directions;
        auto addDirection = [&](int direction) {
            if ((direction == 8 && request.allowedTypes.testFlag(DotType))
                    || (direction < 8 && request.allowedTypes.testFlag(DirectionalType)))
                if (!directions.contains(direction)) directions.append(direction);
        };
        const int preferredDirection = hand == preferred.hand ? preferred.direction : mirrored(preferred.direction);
        addDirection(preferredDirection);
        // A motif may enter with the reverse stroke while keeping its swing
        // axis. Previously only the last note's opposite was available, which
        // could unnecessarily turn a diagonal theme into a vertical reset.
        addDirection(opposite(preferredDirection));
        if (state.hands[hand].present) addDirection(opposite(state.hands[hand].note.direction));
        // Adjacent diagonal reversals retain the phrase's orientation when an
        // entry from the previous segment makes its exact stroke unsuitable.
        if (preferredDirection == 0 || preferredDirection == 4 || preferredDirection == 5) {
            addDirection(4); addDirection(5);
        } else if (preferredDirection == 1 || preferredDirection == 6 || preferredDirection == 7) {
            addDirection(6); addDirection(7);
        } else if (preferredDirection == 2) {
            addDirection(4); addDirection(6);
        } else if (preferredDirection == 3) {
            addDirection(5); addDirection(7);
        }
        addDirection(1); addDirection(0);
        if (preferred.direction == 8) addDirection(8);
        const int home = hand == 0 ? 1 : 2;
        const int preferredX = hand == preferred.hand ? preferred.x : 3-preferred.x;
        QVector<QPoint> positions{{preferredX,preferred.y}, {home,qMin(1,preferred.y)}};
        if (state.hands[hand].present) positions.append({state.hands[hand].note.x,state.hands[hand].note.y});
        positions.append({home,1});
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
        // Safe motion is part of the desired phrase. Penalising every unit of
        // travel made an in-place reversal cheaper than any change of cell.
        // Keep the hard speed limit, with a soft cost near its boundary only.
        const double strain = qMax(0.0,(distance/qMax(.15,allowance)-.55)/.45);
        cost += .8*strain*strain;
        if (previous.note.direction != 8 && note.direction != 8) {
            const double agreement = dot(cutVector(previous.note.direction),cutVector(note.direction));
            // Fast sequences need a reversal rather than an invisible reset.
            if (dt <= 1.0+epsilon && agreement > .1) return false;
            cost += qMax(0.0,agreement+.4)*(dt <= 1.5 ? 2.5 : .6);
        }
    }
    cost += note.color == preferred.hand ? 0.0 : preferred.keepAxis ? 2.7 : 1.2;
    const int desiredDirection=note.color == preferred.hand ? preferred.direction : mirrored(preferred.direction);
    cost += note.direction == desiredDirection ? 0.0 : 1.0;
    if (preferred.keepAxis && note.direction!=desiredDirection && note.direction!=opposite(desiredDirection)) cost+=1.5;
    cost += (preferred.keepAxis ? 1.6 : .85)*(std::abs(note.x-(note.color == preferred.hand ? preferred.x : 3-preferred.x))
                  +std::abs(note.y-preferred.y));
    if (source.lastHand == note.color && seconds-source.lastSeconds < .6) cost += .6;
    const Action action{note.color, note.direction, note.x, note.y};
    // Repeated four/eight-stroke strings are a soft quality cost. Safe entry
    // and exit conditions above always win over avoiding a familiar gesture.
    for (int period : {4, 8, 16}) {
        if (source.recentActions.size() < period + 3) continue;
        const int end = source.recentActions.size();
        bool repeats = sameAction(action, source.recentActions[end - period]);
        for (int prior = 1; prior <= 3 && repeats; ++prior)
            repeats = sameAction(source.recentActions[end - prior], source.recentActions[end - period - prior]);
        if (repeats) cost += (period == 4 ? 1.0 : .7)*(preferred.keepAxis ? .15 : 1.0);
    }
    *target = source;
    target->hands[note.color] = {true,note,seconds};
    ++target->counts[note.color];
    cost += .03*std::abs(target->counts[0]-target->counts[1]);
    target->lastHand = note.color; target->lastSeconds = seconds; target->cost += cost;
    target->recentActions.append(action);
    if (target->recentActions.size() > 32) target->recentActions.removeFirst();
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
    for (int i=0; i<analysis.anchors.size(); ++i) {
        if ((i & 255)==0 && stopped()) return fail(QStringLiteral("已取消本地制谱。"));
        const auto &anchor=analysis.anchors[i];
        if (!validAnchor(anchor,request,analysis)) return fail(QStringLiteral("音乐锚点与当前拍线不一致，请重新分析。"));
        if (anchor.kind==MusicAnchorKind::Hit) ++hitCandidates;
    }
    double previousEnd=-1.0;
    QSet<QString> segmentIds;
    for (const auto &segment : analysis.segments) {
        const double segmentStartSeconds=request.timeMap.beatToSeconds(segment.startBeat);
        const double segmentEndSeconds=request.timeMap.beatToSeconds(segment.endBeat);
        if (!std::isfinite(segment.startBeat) || !std::isfinite(segment.endBeat)
                || segment.startBeat<0 || segment.endBeat<=segment.startBeat
                || !std::isfinite(segment.energy) || segment.energy<0
                || !std::isfinite(segment.activeSeconds) || segment.activeSeconds<0
                || segment.id.isEmpty() || segmentIds.contains(segment.id)
                || !std::isfinite(segmentStartSeconds) || !std::isfinite(segmentEndSeconds)
                || segmentStartSeconds < -epsilon || segmentEndSeconds > analysis.durationSeconds+epsilon
                || segment.activeSeconds > segmentEndSeconds-segmentStartSeconds+epsilon
                || segment.startBeat<previousEnd-epsilon || segment.anchors.size()>128)
            return fail(QStringLiteral("音乐段落范围无效，请重新分析。"));
        for (int index : segment.anchors)
            if (index<0 || index>=analysis.anchors.size()) return fail(QStringLiteral("音乐段落引用了无效锚点。"));
        previousEnd=segment.endBeat; segmentIds.insert(segment.id);
    }

    GenerationDraft result;
    result.source=request; result.warnings=analysis.warnings;
    MusicAnalysis phrasedAnalysis = analysis;
    // Rebuild from already validated anchors rather than trusting cached or
    // externally supplied phrase ranges/indices after a source revision.
    phrasedAnalysis.rebuildPhrases(request.timeMap);
    if (request.arrangement) {
        SongArrangementPlan checked;
        QStringList planErrors;
        if ((request.arrangement->source != "local" && request.arrangement->source != "ai")
                || !SongArrangementPlanner::parsePlan(request.arrangement->toJson(), request, phrasedAnalysis, &checked, &planErrors))
            return fail(QStringLiteral("整曲编排与当前音乐或难度不一致，请重新获取建议。\n%1").arg(planErrors.join('\n')));
        result.arrangement = request.arrangement;
    } else result.arrangement = std::make_shared<SongArrangementPlan>(SongArrangementPlanner::localPlan(request, phrasedAnalysis));
    result.source.arrangement = result.arrangement;
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
    QVector<int> recentFamilies;
    std::array<int, 6> familyCounts{};
    const quint32 seed = musicSeed(request, analysis);
    QSet<qint64> usedHitBeats;
    int skippedForMovement=0, repeatedAdaptations=0, previousDotHand=-1, phraseCount=0;
    QString lastPhraseSignature;
    int identicalPhrases=0, longestIdenticalPhrases=0;
    if (request.allowedTypes.testFlag(DirectionalType) || request.allowedTypes.testFlag(DotType)) {
        for (int segmentIndex=0; segmentIndex<analysis.segments.size(); ++segmentIndex) {
            if (stopped()) return fail(QStringLiteral("已取消本地制谱。"));
            const auto &segment=analysis.segments[segmentIndex];
            if (segment.activeSeconds<=0 || segment.energy<=1e-6) continue;
            const SectionArrangement *section = result.arrangement->section(segment.id);
            if (!section || section->rest || section->targetNps <= 0) continue;
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
                const double rhythmWeight = section->rhythm == RhythmStrategy::Syncopated
                    ? 1.0 - metrical * .7 : section->rhythm == RhythmStrategy::StrongBeats ? metrical : .65;
                ranked.append({&anchor,anchor.seconds,.6*qBound(0.0,anchor.strength,1.0)
                    +.25*qBound(0.0,anchor.confidence,1.0)+.15*rhythmWeight});
            }
            if (ranked.size()>128) return fail(QStringLiteral("单段音乐起音过多，请重新分析拍格。"));
            std::stable_sort(ranked.begin(),ranked.end(),[](const Hit &a,const Hit &b) {
                return a.score!=b.score ? a.score>b.score : a.seconds<b.seconds;
            });
            const double target=qBound(0.0,section->targetNps,profile.targetMaxNps);
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
            const QString key=section->motifId.isEmpty() ? segment.repeatGroup : section->motifId;
            const bool repeated = themes.contains(key) && !themes.value(key).core.isEmpty();
            const Theme reference = themes.value(key);
            Theme realisedCore;
            for (int phraseIndex=0; phraseIndex<phrasedAnalysis.phrases.size(); ++phraseIndex) {
                const auto &phrase=phrasedAnalysis.phrases[phraseIndex];
                if (phrase.segmentIndex != segmentIndex) continue;
                QVector<Hit> phraseHits;
                for (const auto &hit : selected)
                    if (hit.anchor->beat >= phrase.startBeat - epsilon && hit.anchor->beat < phrase.endBeat - epsilon) phraseHits.append(hit);
                if (phraseHits.isEmpty()) continue;
                const bool corePhrase = phrase.startBeat < segment.startBeat + 4 - epsilon;
                PhraseTheme theme = phraseTheme(*section,phrase,recentFamilies,seed,phraseIndex,request,
                    repeated && corePhrase ? &reference : nullptr);
                if (phraseHits.size() > 1) {
                    QVector<double> gaps;
                    for (int i=1; i<phraseHits.size(); ++i) gaps.append(phraseHits[i].anchor->beat-phraseHits[i-1].anchor->beat);
                    std::sort(gaps.begin(),gaps.end()); theme.strokeBeats = qBound(.25,gaps[gaps.size()/2],2.0);
                }
                QVector<Action> preferred;
                int preferredHand = state.lastHand;
                for (const auto &hit : phraseHits) {
                    Action action=expectedAction(theme,hit,request,preferredHand);
                    const qint64 phase=qRound64((hit.anchor->beat-segment.startBeat)*1000000);
                    if (repeated && hit.anchor->beat < segment.startBeat + 4 - epsilon && reference.core.contains(phase)) {
                        action=reference.core.value(phase);
                        action.keepAxis=true;
                        if (!request.allowedTypes.testFlag(DotType) && action.direction==8) action.direction=1;
                        if (!request.allowedTypes.testFlag(DirectionalType)) action.direction=8;
                    }
                    preferredHand=action.hand; preferred.append(action);
                }
                if (request.allowedTypes.testFlag(DotType) && request.allowedTypes.testFlag(DirectionalType)) {
                    QVector<int> accents;
                    double maximumStrength=0;
                    for (const auto &hit : phraseHits) maximumStrength=qMax(maximumStrength,hit.anchor->strength);
                    for (int i=0; i<phraseHits.size(); ++i) {
                        const auto &hit=phraseHits[i];
                        const bool ending=phrase.endBeat-hit.anchor->beat <= qMax(.5,theme.strokeBeats)+epsilon;
                        if (hit.anchor->strength >= qMax(.25,maximumStrength*.8)
                                || (ending && hit.anchor->strength >= .2 && section->development==ThemeDevelopment::Close)) accents.append(i);
                        if (preferred[i].direction==8) preferred[i].direction=expectedAction(theme,hit,request,i ? preferred[i-1].hand : state.lastHand).direction;
                    }
                    std::stable_sort(accents.begin(),accents.end(),[&](int a,int b) {
                        if (phraseHits[a].anchor->strength!=phraseHits[b].anchor->strength)
                            return phraseHits[a].anchor->strength>phraseHits[b].anchor->strength;
                        return phraseHits[a].anchor->beat>phraseHits[b].anchor->beat;
                    });
                    const int accentBudget=qMax(1,int(std::floor(phraseHits.size()*.15)));
                    for (int i=0; i<qMin(accentBudget,accents.size()); ++i) {
                        auto &action=preferred[accents[i]];
                        const int balancedHand=previousDotHand<0 ? theme.leadHand : 1-previousDotHand;
                        if (balancedHand!=action.hand) { action.hand=balancedHand; action.x=3-action.x; }
                        action.direction=8; action.keepAxis=false; previousDotHand=balancedHand;
                    }
                }
                QString signature;
                int placed=0;
                for (int i=0; i<phraseHits.size(); ++i) {
                    bool searchStopped=false;
                    const Decision decision=searchNext(phraseHits,preferred,i,state,request,cancelled,&searchStopped);
                    if (searchStopped) return fail(QStringLiteral("已取消本地制谱。"));
                    if (!decision.place) { ++skippedForMovement; continue; }
                    SearchState next;
                    if (!advanceState(state,decision.note,phraseHits[i].seconds,preferred[i],profile,&next))
                        return fail(QStringLiteral("本地动作衔接状态失效，未生成草稿。"));
                    next.cost=0; state=next; ++placed;
                    notes.append(decision.note); noteTimes.append(phraseHits[i].seconds);
                    usedHitBeats.insert(qRound64(decision.note.beat*1000000));
                    const Action action{decision.note.color,decision.note.direction,decision.note.x,decision.note.y};
                    signature+=QStringLiteral("%1,%2,%3,%4,%5;").arg(decision.note.beat-phrase.startBeat,0,'f',3)
                        .arg(action.hand).arg(action.direction).arg(action.x).arg(action.y);
                    if (decision.note.beat < segment.startBeat + 4 - epsilon)
                        realisedCore.core.insert(qRound64((decision.note.beat-segment.startBeat)*1000000),action);
                    if (repeated && !sameAction(action,preferred[i])) ++repeatedAdaptations;
                }
                if (placed) {
                    if (corePhrase) realisedCore.family=theme.family;
                    ++familyCounts[int(theme.family)]; ++phraseCount;
                    recentFamilies.append(int(theme.family)); if (recentFamilies.size()>4) recentFamilies.removeFirst();
                    identicalPhrases=signature==lastPhraseSignature ? identicalPhrases+1 : 1;
                    longestIdenticalPhrases=qMax(longestIdenticalPhrases,identicalPhrases); lastPhraseSignature=signature;
                }
            }
            if (!repeated && !realisedCore.core.isEmpty()) themes.insert(key,realisedCore);
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
    for (int count : familyCounts) result.metrics.actionFamilyCounts.append(count);
    result.metrics.longestRepeatedPhraseRun=longestIdenticalPhrases;
    if (skippedForMovement) result.warnings.append(QStringLiteral("为保证同手间隔和挥刀衔接，跳过了 %1 个起音候选。").arg(skippedForMovement));
    if (repeatedAdaptations) result.warnings.append(QStringLiteral("重复段中 %1 个动作根据当前起音和两手衔接作了调整。").arg(repeatedAdaptations));
    if (!notes.isEmpty() && result.metrics.averageNps+epsilon<profile.targetMinNps)
        result.warnings.append(QStringLiteral("可靠起音不足以达到难度的目标密度；已保留真实音乐节奏，未补造起音。"));
    result.warnings.append(QStringLiteral("本地动作校验是桌面近似，新增制谱方式的实际手感仍需头显游玩确认。"));
    const QStringList familyLabels{QStringLiteral("垂直收放"),QStringLiteral("斜切收放"),QStringLiteral("横向往返"),
        QStringLiteral("上下阶梯"),QStringLiteral("内外展开"),QStringLiteral("左右问答")};
    QStringList usedFamilies;
    for (int i=0; i<6; ++i) if (familyCounts[i]) usedFamilies.append(QStringLiteral("%1 %2 句").arg(familyLabels[i]).arg(familyCounts[i]));
    result.summary=QStringLiteral("本地音乐编排：%1；按重音、疏密、切分和频带线索安排 %2 个短乐句，重复段保留前四拍主题核心并发展。动作族：%3；连续完全相同乐句最长 %4 句；编排种子 %5。共 %6 个方向块、%7 个无方向块、%8 个炸弹、%9 个侧墙。")
        .arg(result.arrangement->source=="ai" ? QStringLiteral("采用 AI 整曲建议") : QStringLiteral("采用本地规划，未调用大语言模型"))
        .arg(phraseCount).arg(usedFamilies.join(QStringLiteral("、"))).arg(longestIdenticalPhrases).arg(request.arrangementSeed)
        .arg(result.metrics.directional).arg(result.metrics.dots).arg(result.metrics.bombs).arg(result.metrics.walls);
    *draft=std::move(result);
    if (progress) progress(100);
    return true;
}

} // namespace lmsc
