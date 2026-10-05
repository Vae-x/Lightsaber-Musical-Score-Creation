#pragma once
#include "AiGenerationService.h"

namespace lmsc {
inline bool sameGenerationSource(const GenerationRequest &a, const GenerationRequest &b, bool compareJob = true) {
    if ((compareJob && a.jobId != b.jobId) || a.documentId != b.documentId || a.difficultyId != b.difficultyId
            || a.documentRevision != b.documentRevision || a.audioRevision != b.audioRevision
            || a.audio.path != b.audio.path || a.audio.sourcePath != b.audio.sourcePath
            || a.audio.revision != b.audio.revision || a.audio.sampleRate != b.audio.sampleRate
            || a.audio.channels != b.audio.channels || a.audio.durationSeconds != b.audio.durationSeconds
            || a.timeMap.baseBpm() != b.timeMap.baseBpm() || a.timeMap.firstBeatSeconds() != b.timeMap.firstBeatSeconds()
            || a.timeMap.changes().size() != b.timeMap.changes().size()
            || a.profile.name != b.profile.name || a.profile.rank != b.profile.rank
            || a.profile.targetMinNps != b.profile.targetMinNps || a.profile.targetMaxNps != b.profile.targetMaxNps
            || a.profile.maxPeakNps != b.profile.maxPeakNps || a.profile.minSameHandGapSeconds != b.profile.minSameHandGapSeconds
            || a.profile.maxConnectionSpeed != b.profile.maxConnectionSpeed || a.profile.subdivision != b.profile.subdivision
            || a.allowedTypes != b.allowedTypes || a.analysisOnly != b.analysisOnly || a.arrangementSeed != b.arrangementSeed
            || bool(a.arrangement) != bool(b.arrangement)) return false;
    for (int i=0; i<a.timeMap.changes().size(); ++i)
        if (a.timeMap.changes()[i].beat != b.timeMap.changes()[i].beat
                || a.timeMap.changes()[i].bpm != b.timeMap.changes()[i].bpm) return false;
    return !a.arrangement || a.arrangement->toJson() == b.arrangement->toJson();
}
}
