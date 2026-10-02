#include "MusicFeatureAnalyzer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QtEndian>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>

namespace lmsc {
namespace {
struct Frame { double seconds, energy, low, mid, high, flux; };
constexpr double pi = 3.14159265358979323846;
QString kindName(MusicAnchorKind kind) {
    return kind == MusicAnchorKind::Hit ? QStringLiteral("hit")
         : kind == MusicAnchorKind::Rest ? QStringLiteral("rest") : QStringLiteral("boundary");
}
QJsonObject anchorJson(const MusicAnchor &anchor) {
    return {{"id", anchor.id}, {"kind", kindName(anchor.kind)}, {"beat", anchor.beat},
            {"seconds", anchor.seconds}, {"strength", anchor.strength}, {"confidence", anchor.confidence},
            {"bands", QJsonArray{anchor.low, anchor.mid, anchor.high}}};
}
void addAnchor(MusicAnalysis *analysis, const MusicAnchor &anchor) {
    if (analysis->anchorIndex.contains(anchor.id)) return;
    analysis->anchorIndex.insert(anchor.id, analysis->anchors.size());
    analysis->anchors.append(anchor);
}
QString boundaryId(double beat) { return QStringLiteral("b%1").arg(qRound64(beat * 1000000.0)); }
double similarity(const QVector<double> &a, const QVector<double> &b) {
    double dot = 0.0, aa = 0.0, bb = 0.0;
    for (int i = 0; i < qMin(a.size(), b.size()); ++i) {
        dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i];
    }
    return aa > 1e-12 && bb > 1e-12 ? dot / std::sqrt(aa * bb) : 0.0;
}
}

const MusicAnchor *MusicAnalysis::anchor(const QString &id) const {
    const int index = anchorIndex.value(id, -1);
    return index >= 0 && index < anchors.size() ? &anchors[index] : nullptr;
}

QJsonObject MusicAnalysis::planningEvidence() const {
    QJsonArray blocks;
    for (const auto &segment : segments) {
        int hits = 0;
        for (int index : segment.anchors) if (anchors[index].kind == MusicAnchorKind::Hit) ++hits;
        blocks.append(QJsonObject{{"segmentId", segment.id}, {"startAnchor", segment.startAnchor},
                      {"endAnchor", segment.endAnchor}, {"startBeat", segment.startBeat},
                      {"endBeat", segment.endBeat}, {"startSeconds", segment.startSeconds},
                      {"endSeconds", segment.endSeconds}, {"energy", segment.energy},
                      {"activeSeconds", segment.activeSeconds}, {"hitCandidates", hits},
                      {"repeatGroup", segment.repeatGroup}, {"repeatConfidence", segment.repeatConfidence}});
    }
    return {{"durationSeconds", durationSeconds}, {"activeSeconds", activeSeconds}, {"blocks", blocks}};
}

QJsonObject MusicAnalysis::segmentEvidence(int index) const {
    QJsonArray candidates;
    if (index < 0 || index >= segments.size()) return {};
    const auto &segment = segments[index];
    for (int anchorIndex : segment.anchors) candidates.append(anchorJson(anchors[anchorIndex]));
    return {{"segmentId", segment.id}, {"startBeat", segment.startBeat}, {"endBeat", segment.endBeat},
            {"startSeconds", segment.startSeconds}, {"endSeconds", segment.endSeconds},
            {"energy", segment.energy}, {"repeatGroup", segment.repeatGroup}, {"anchors", candidates}};
}

bool MusicFeatureAnalyzer::analyze(const GenerationRequest &request, MusicAnalysis *analysis,
                                 QString *error, const std::function<bool()> &cancelled,
                                 const std::function<void(int)> &progress) {
    if (error) error->clear();
    if (!analysis) return false;
    *analysis = {};
    auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    auto stopped = [&] { return cancelled && cancelled(); };
    const auto &audio = request.audio;
    if (!audio.isValid() || !std::isfinite(audio.durationSeconds) || audio.sampleRate < 1000
            || audio.sampleRate > 384000 || audio.channels < 1 || audio.channels > 16
            || request.profile.subdivision < 1 || request.profile.subdivision > 4)
        return fail(QStringLiteral("原速 PCM 音频或难度采样参数无效。"));
    QFile file(audio.path);
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("无法读取原速 PCM 音频。"));
    const QFileInfo initialInfo(file);
    const auto initialModified = initialInfo.lastModified();
    const qint64 initialSize = file.size();
    const int frameBytes = 2 * audio.channels;
    if (initialSize <= 0 || initialSize % frameBytes != 0)
        return fail(QStringLiteral("PCM 数据为空或帧长度不完整。"));
    const qint64 totalFrames = initialSize / frameBytes;
    const double duration = totalFrames / double(audio.sampleRate);
    if (duration > 3600.0) return fail(QStringLiteral("音频超过一小时，当前整曲分析上限为一小时。"));
    if (std::abs(duration - audio.durationSeconds) > qMax(0.05, 2.0 / audio.sampleRate))
        return fail(QStringLiteral("PCM 时长与当前音频快照不一致，请重新载入歌曲。"));
    const double startBeat = qMax(0.0, request.timeMap.secondsToBeat(0.0));
    const double endBeat = request.timeMap.secondsToBeat(duration);
    if (!std::isfinite(startBeat) || !std::isfinite(endBeat) || endBeat <= startBeat || endBeat > 10000000.0)
        return fail(QStringLiteral("当前拍线与音频没有有效的可生成范围。"));
    const int hop = qMax(1, audio.sampleRate / 100);
    const double lowAlpha = 1.0 - std::exp(-2 * pi * 200.0 / audio.sampleRate);
    const double highAlpha = 1.0 - std::exp(-2 * pi * 2000.0 / audio.sampleRate);
    std::array<double, 16> lowState{}, highState{};
    double energySum = 0.0, lowSum = 0.0, midSum = 0.0, highSum = 0.0;
    int count = 0;
    qint64 frameNumber = 0;
    QVector<Frame> frames;
    frames.reserve(static_cast<int>(totalFrames / hop + 1));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    int lastProgress = -1;
    auto finishHop = [&] {
        if (!count) return;
        const double divisor = count * audio.channels;
        frames.append({(frameNumber - count * 0.5) / audio.sampleRate,
                       std::sqrt(energySum / divisor), std::sqrt(lowSum / divisor),
                       std::sqrt(midSum / divisor), std::sqrt(highSum / divisor), 0.0});
        energySum = lowSum = midSum = highSum = 0.0;
        count = 0;
    };
    while (!file.atEnd()) {
        if (stopped()) return fail(QStringLiteral("已取消音乐分析。"));
        const QByteArray bytes = file.read((65536 / frameBytes) * frameBytes);
        if (bytes.isEmpty()) return fail(QStringLiteral("PCM 读取中断，无法完成整曲分析。"));
        hash.addData(bytes);
        for (int offset = 0; offset + frameBytes <= bytes.size(); offset += frameBytes) {
            for (int channel = 0; channel < audio.channels; ++channel) {
                const double sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(bytes.constData() + offset + 2 * channel)) / 32768.0;
                lowState[channel] += lowAlpha * (sample - lowState[channel]);
                highState[channel] += highAlpha * (sample - highState[channel]);
                const double low = lowState[channel], mid = highState[channel] - low;
                const double high = sample - highState[channel];
                energySum += sample * sample; lowSum += low * low; midSum += mid * mid; highSum += high * high;
            }
            ++frameNumber; ++count;
            if (count == hop) finishHop();
        }
        const int percent = static_cast<int>(file.pos() * 65 / initialSize);
        if (percent != lastProgress && progress) { progress(percent); lastProgress = percent; }
    }
    finishHop();
    const QFileInfo finalInfo(audio.path);
    if (finalInfo.size() != initialSize || finalInfo.lastModified() != initialModified)
        return fail(QStringLiteral("音频在分析期间发生变化，请重新生成。"));
    if (frames.size() < 3) return fail(QStringLiteral("音频太短，无法提取可靠音乐证据。"));
    double maximumEnergy = 0.0, maximumFlux = 0.0;
    double lowBase = frames[0].low, midBase = frames[0].mid, highBase = frames[0].high;
    for (auto &frame : frames) {
        maximumEnergy = qMax(maximumEnergy, frame.energy);
        frame.flux = qMax(0.0, frame.low - lowBase) + qMax(0.0, frame.mid - midBase)
                   + qMax(0.0, frame.high - highBase);
        maximumFlux = qMax(maximumFlux, frame.flux);
        lowBase = lowBase * 0.8 + frame.low * 0.2;
        midBase = midBase * 0.8 + frame.mid * 0.2;
        highBase = highBase * 0.8 + frame.high * 0.2;
    }
    if (maximumEnergy < 1e-5) return fail(QStringLiteral("音频近乎静音，无法生成有音乐依据的曲谱。"));
    analysis->durationSeconds = duration;
    analysis->audioFingerprint = QString::fromLatin1(hash.result().toHex());
    const double hopSeconds = hop / double(audio.sampleRate);
    // Musical activity spans include the short gaps between drum strikes. Raw
    // 10 ms energetic frames would severely inflate NPS for sparse percussion.
    QVector<QPair<double,double>> activeSpans;
    for (const auto &frame : frames) {
        if (frame.energy <= maximumEnergy * 0.025) continue;
        const double from = qMax(0.0, frame.seconds-hopSeconds*.5);
        const double to = qMin(duration, frame.seconds+hopSeconds*.5);
        if (!activeSpans.isEmpty()) {
            auto &last = activeSpans.last();
            const double bpm = request.timeMap.bpmAtBeat(request.timeMap.secondsToBeat(last.second));
            const double mergeGap = qMin(2.0, qMax(1.0, 2.0*60.0/bpm));
            if (from-last.second <= mergeGap) { last.second=to; continue; }
        }
        activeSpans.append(qMakePair(from,to));
    }
    for (auto &span : activeSpans) {
        span.first=qMax(0.0,span.first-.08); span.second=qMin(duration,span.second+.08);
        analysis->activeSeconds += span.second-span.first;
    }
    analysis->activeSeconds=qMin(duration,analysis->activeSeconds);
    QMap<qint64, int> hitFrames;
    int detected = 0, rejected = 0;
    double fluxBaseline = 0.0;
    for (int i = 1; i + 1 < frames.size(); ++i) {
        if ((i & 2047) == 0 && stopped()) return fail(QStringLiteral("已取消音乐分析。"));
        const auto &frame = frames[i];
        const double threshold = qMax(maximumFlux * 0.04, fluxBaseline * 1.4);
        fluxBaseline = fluxBaseline * 0.98 + frame.flux * 0.02;
        if (frame.flux <= threshold || frame.flux < frames[i-1].flux || frame.flux <= frames[i+1].flux) continue;
        ++detected;
        const double rawBeat = request.timeMap.secondsToBeat(frame.seconds);
        const qint64 tick = qRound64(rawBeat * request.profile.subdivision);
        const double beat = tick / double(request.profile.subdivision);
        const double seconds = request.timeMap.beatToSeconds(beat);
        const double tolerance = qMin(0.08, 0.12 * 60.0 / request.timeMap.bpmAtBeat(beat));
        if (tick < 0 || seconds < 0 || seconds >= duration || std::abs(seconds - frame.seconds) > tolerance) {
            ++rejected; continue;
        }
        if (!hitFrames.contains(tick) || frames[hitFrames.value(tick)].flux < frame.flux) hitFrames.insert(tick, i);
    }
    if (detected > 0 && rejected > detected * 0.35)
        analysis->warnings.append(QStringLiteral("较多起音偏离当前拍线；请试听并确认 BPM、偏移或变速，再评价生成结果。"));
    const qint64 firstTick = qCeil(startBeat * request.profile.subdivision);
    const qint64 lastTick = qFloor(endBeat * request.profile.subdivision);
    if (lastTick - firstTick > 250000) return fail(QStringLiteral("拍格数量超过分析上限，请确认 BPM。"));
    for (qint64 tick = firstTick; tick <= lastTick; ++tick) {
        if ((tick & 2047) == 0 && stopped()) return fail(QStringLiteral("已取消音乐分析。"));
        const double beat = tick / double(request.profile.subdivision);
        const double seconds = request.timeMap.beatToSeconds(beat);
        if (seconds < 0 || seconds >= duration) continue;
        const int frameIndex = qBound(0, qRound(seconds / hopSeconds), frames.size() - 1);
        const auto &frame = frames[hitFrames.value(tick, frameIndex)];
        if (hitFrames.contains(tick)) {
            addAnchor(analysis, {QStringLiteral("h%1").arg(tick), MusicAnchorKind::Hit, seconds, beat,
                      frame.flux / qMax(1e-12, maximumFlux),
                      qBound(0.0, 1.0 - std::abs(frame.seconds - seconds) / 0.1, 1.0),
                      frame.low / maximumEnergy, frame.mid / maximumEnergy, frame.high / maximumEnergy});
        } else if (frame.energy > maximumEnergy * 0.025) {
            const auto next = hitFrames.lowerBound(tick);
            double distance = 1e9;
            if (next != hitFrames.end()) distance = std::abs(request.timeMap.beatToSeconds(next.key() / double(request.profile.subdivision)) - seconds);
            if (next != hitFrames.begin()) { auto prior = next; --prior; distance = qMin(distance, std::abs(request.timeMap.beatToSeconds(prior.key() / double(request.profile.subdivision)) - seconds)); }
            if (distance >= 0.18) addAnchor(analysis, {QStringLiteral("r%1").arg(tick), MusicAnchorKind::Rest,
                          seconds, beat, frame.energy / maximumEnergy, 1.0,
                          frame.low / maximumEnergy, frame.mid / maximumEnergy, frame.high / maximumEnergy});
        }
        if (tick % request.profile.subdivision == 0)
            addAnchor(analysis, {boundaryId(beat), MusicAnchorKind::Boundary, seconds, beat, 0.0, 1.0});
    }
    double cursor = startBeat;
    while (cursor < endBeat - 1e-7) {
        if (stopped()) return fail(QStringLiteral("已取消音乐分析。"));
        const double cursorSeconds = request.timeMap.beatToSeconds(cursor);
        double next = qMin(endBeat, qMin(cursor + 16.0, request.timeMap.secondsToBeat(cursorSeconds + 16.0)));
        if (next < endBeat - 1e-7) next = std::floor(next * request.profile.subdivision) / request.profile.subdivision;
        if (next <= cursor + 1e-7) return fail(QStringLiteral("拍线切分失败，请确认时间参数。"));
        MusicSegment segment;
        segment.id = QStringLiteral("s%1").arg(analysis->segments.size());
        segment.startBeat = cursor; segment.endBeat = next;
        segment.startSeconds = cursorSeconds; segment.endSeconds = request.timeMap.beatToSeconds(next);
        segment.startAnchor = boundaryId(cursor); segment.endAnchor = boundaryId(next);
        addAnchor(analysis, {segment.startAnchor, MusicAnchorKind::Boundary, cursorSeconds, cursor, 0.0, 1.0});
        addAnchor(analysis, {segment.endAnchor, MusicAnchorKind::Boundary, segment.endSeconds, next, 0.0, 1.0});
        segment.fingerprint.fill(0.0, 20);
        const int from = qBound(0, static_cast<int>(cursorSeconds / hopSeconds), frames.size()-1);
        const int to = qBound(from+1, qCeil(segment.endSeconds / hopSeconds), frames.size());
        for (int i = from; i < to; ++i) {
            const auto &frame = frames[i];
            segment.energy += frame.energy / maximumEnergy;
            const int bin = qBound(0, static_cast<int>((frame.seconds-cursorSeconds) * 16.0 / (segment.endSeconds-cursorSeconds)), 15);
            segment.fingerprint[bin] += frame.flux / qMax(1e-12, maximumFlux);
            segment.fingerprint[16] += frame.low / maximumEnergy;
            segment.fingerprint[17] += frame.mid / maximumEnergy;
            segment.fingerprint[18] += frame.high / maximumEnergy;
        }
        segment.energy /= qMax(1, to-from);
        for (const auto &span : activeSpans)
            segment.activeSeconds += qMax(0.0,qMin(span.second,segment.endSeconds)-qMax(span.first,cursorSeconds));
        for (int i = 0; i < analysis->anchors.size(); ++i) {
            const auto &anchor = analysis->anchors[i];
            if (anchor.beat >= cursor - 1e-7 && (anchor.beat < next - 1e-7
                    || (anchor.kind == MusicAnchorKind::Boundary && anchor.beat <= next + 1e-7)))
                segment.anchors.append(i);
        }
        // At subdivision <=4, a normal 16-beat block has <=82 anchors. Reject
        // future oversized profiles rather than dropping musical evidence.
        if (segment.anchors.size() > 128) {
            return fail(QStringLiteral("单段音乐证据超过上限，请降低拍格密度。"));
        }
        // Give rhythmic distribution its own normalization; otherwise summed
        // spectral energy overwhelms onset shape and unrelated rhythms match.
        double fluxNorm=0.0,bandNorm=0.0;
        for (int i=0;i<16;++i) fluxNorm+=segment.fingerprint[i]*segment.fingerprint[i];
        for (int i=16;i<19;++i) bandNorm+=segment.fingerprint[i]*segment.fingerprint[i];
        for (int i=0;i<16;++i) segment.fingerprint[i]/=qMax(1e-12,std::sqrt(fluxNorm));
        for (int i=16;i<19;++i) segment.fingerprint[i]=segment.fingerprint[i]/qMax(1e-12,std::sqrt(bandNorm))*.5;
        segment.fingerprint[19]=segment.energy*.25;
        segment.repeatGroup = QStringLiteral("r%1").arg(analysis->segments.size());
        for (const auto &prior : analysis->segments) {
            if (std::abs((prior.endBeat-prior.startBeat) - (next-cursor)) > 0.5) continue;
            const double score = similarity(segment.fingerprint, prior.fingerprint);
            if (score >= 0.94 && std::abs(segment.energy-prior.energy) <= 0.2) {
                segment.repeatGroup = prior.repeatGroup; segment.repeatConfidence = score; break;
            }
        }
        analysis->segments.append(segment);
        cursor = next;
    }
    if (hitFrames.isEmpty()) analysis->warnings.append(QStringLiteral("未找到可绑定当前拍格的可靠起音，请检查节拍对齐。"));
    if (progress) progress(100);
    return !stopped();
}

} // namespace lmsc
