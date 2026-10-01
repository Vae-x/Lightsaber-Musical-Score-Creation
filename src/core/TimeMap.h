#pragma once

#include <QVector>
#include <QString>

namespace lmsc {

struct TempoChange {
    double beat = 0.0;
    double bpm = 120.0;
};

// A continuous, invertible tempo map. Offset is the audio time of beat zero.
class TimeMap {
public:
    bool configure(double initialBpm, double offsetSeconds,
                   const QVector<TempoChange> &changes = {}, QString *error = nullptr);
    double beatToSeconds(double beat) const;
    double secondsToBeat(double seconds) const;
    double bpmAtBeat(double beat) const;
    double initialBpm() const { return m_initialBpm; }
    double offsetSeconds() const { return m_offset; }
    double baseBpm() const { return m_initialBpm; }
    double firstBeatSeconds() const { return m_offset; }
    const QVector<TempoChange> &changes() const { return m_changes; }

private:
    struct Segment { double beat, seconds, bpm; };
    double m_initialBpm = 120.0;
    double m_offset = 0.0;
    QVector<TempoChange> m_changes;
    QVector<Segment> m_segments{{0.0, 0.0, 120.0}};
};

} // namespace lmsc
