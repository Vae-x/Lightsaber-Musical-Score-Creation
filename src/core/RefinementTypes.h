#pragma once

#include "AiGenerationService.h"

namespace lmsc {
struct RefinementPatch {
    QVector<BeatObject> updates, additions;
    QStringList removals;
    bool isEmpty() const { return updates.isEmpty() && additions.isEmpty() && removals.isEmpty(); }
};
struct RefinementRequest {
    GenerationRequest generation;
    QVector<BeatObject> baseline;
    QString baselineHash;
    double startSeconds = 0.0, endSeconds = 0.0;
    bool selectedOnly = false;
    int maximumSegments = 5;
};
struct RefinementStats {
    int changed = 0, moved = 0, added = 0, removed = 0;
};
struct RefinementResult {
    RefinementRequest source;
    GenerationDraft candidate;
    RefinementPatch patch;
    RefinementStats stats;
    QStringList processedSegments, remainingSegments;
    QStringList processedRanges;
    bool resumable = false;
};
QString refinementBaselineHash(const QVector<BeatObject> &objects);
RefinementPatch refinementDifference(const QVector<BeatObject> &before, const QVector<BeatObject> &after);
RefinementStats refinementStatistics(const RefinementPatch &patch, const QVector<BeatObject> &before);
}
Q_DECLARE_METATYPE(lmsc::RefinementRequest)
Q_DECLARE_METATYPE(lmsc::RefinementResult)
