#include "TimeMap.h"
#include <algorithm>
#include <cmath>

namespace lmsc {

bool TimeMap::configure(double bpm, double offset, const QVector<TempoChange> &changes,
                        QString *error) {
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!std::isfinite(bpm) || bpm <= 0.0 || !std::isfinite(offset))
        return fail(QStringLiteral("BPM 必须大于零，第一拍时间必须是有限数值。"));
    QVector<TempoChange> sorted = changes;
    for (const auto &change : sorted)
        if (!std::isfinite(change.beat) || change.beat < 0.0 ||
            !std::isfinite(change.bpm) || change.bpm <= 0.0)
            return fail(QStringLiteral("变速事件包含无效的拍数或 BPM。"));
    std::stable_sort(sorted.begin(), sorted.end(), [](const TempoChange &a, const TempoChange &b) {
        return a.beat < b.beat;
    });
    QVector<Segment> segments{{0.0, offset, bpm}};
    for (const auto &change : sorted) {
        auto last = segments.last();
        const double seconds = last.seconds + (change.beat - last.beat) * 60.0 / last.bpm;
        if (change.beat == last.beat) segments.last().bpm = change.bpm;
        else segments.append({change.beat, seconds, change.bpm});
    }
    m_initialBpm = bpm;
    m_offset = offset;
    m_changes = sorted;
    m_segments = segments;
    return true;
}

double TimeMap::beatToSeconds(double beat) const {
    auto it = std::upper_bound(m_segments.begin(), m_segments.end(), beat,
                              [](double value, const Segment &segment) { return value < segment.beat; });
    if (it != m_segments.begin()) --it;
    return it->seconds + (beat - it->beat) * 60.0 / it->bpm;
}

double TimeMap::secondsToBeat(double seconds) const {
    auto it = std::upper_bound(m_segments.begin(), m_segments.end(), seconds,
                              [](double value, const Segment &segment) { return value < segment.seconds; });
    if (it != m_segments.begin()) --it;
    return it->beat + (seconds - it->seconds) * it->bpm / 60.0;
}

double TimeMap::bpmAtBeat(double beat) const {
    auto it = std::upper_bound(m_segments.begin(), m_segments.end(), beat,
                              [](double value, const Segment &segment) { return value < segment.beat; });
    if (it != m_segments.begin()) --it;
    return it->bpm;
}

} // namespace lmsc
