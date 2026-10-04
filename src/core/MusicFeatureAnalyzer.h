#pragma once

#include "AiGenerationService.h"
#include <QHash>
#include <QJsonObject>
#include <functional>

namespace lmsc {

enum class MusicAnchorKind { Hit, Rest, Boundary };
struct MusicAnchor {
    QString id;
    MusicAnchorKind kind = MusicAnchorKind::Boundary;
    double seconds = 0.0, beat = 0.0, strength = 0.0, confidence = 0.0;
    double low = 0.0, mid = 0.0, high = 0.0;
};
struct MusicSegment {
    QString id, startAnchor, endAnchor, repeatGroup;
    double startBeat = 0.0, endBeat = 0.0, startSeconds = 0.0, endSeconds = 0.0;
    double energy = 0.0, activeSeconds = 0.0, repeatConfidence = 0.0;
    QVector<int> anchors;
    QVector<double> fingerprint;
    int repeatReference = -1;
};
// Short musical phrases live inside the stable analysis blocks used by the
// original model generator. Bands describe spectral evidence, not instruments.
struct MusicPhrase {
    QString id;
    int segmentIndex = -1;
    double startBeat = 0.0, endBeat = 0.0, startSeconds = 0.0, endSeconds = 0.0;
    double energy = 0.0, energyTrend = 0.0, hitDensity = 0.0, syncopation = 0.0;
    double restFraction = 0.0, longestGapBeats = 0.0, accentStrength = 0.0;
    double low = 0.0, mid = 0.0, high = 0.0;
    QVector<int> anchors;
};
struct MusicAnalysis {
    QString audioFingerprint;
    double durationSeconds = 0.0, activeSeconds = 0.0;
    QVector<MusicAnchor> anchors;
    QHash<QString, int> anchorIndex;
    QVector<MusicSegment> segments;
    QVector<MusicPhrase> phrases;
    QStringList warnings;
    const MusicAnchor *anchor(const QString &id) const;
    QJsonObject planningEvidence() const;
    QJsonObject segmentEvidence(int index) const;
    void identifyRepeats();
    void rebuildPhrases(const TimeMap &timeMap);
};

class MusicFeatureAnalyzer {
public:
    // Reads the full original-speed interleaved signed little-endian 16-bit PCM.
    // Never uploads audio and never creates beatmap objects.
    static bool analyze(const GenerationRequest &request, MusicAnalysis *analysis,
                        QString *error, const std::function<bool()> &cancelled = {},
                        const std::function<void(int)> &progress = {});
};

} // namespace lmsc
