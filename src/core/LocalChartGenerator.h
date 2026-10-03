#pragma once

#include "MusicFeatureAnalyzer.h"
#include <functional>

namespace lmsc {

// Deterministic local arrangement. All timings originate from analysed anchors;
// this class neither reads devices nor changes a document or the source audio.
class LocalChartGenerator {
public:
    static bool generate(const GenerationRequest &request, const MusicAnalysis &analysis,
                         GenerationDraft *draft, QString *error,
                         const std::function<bool()> &cancelled = {},
                         const std::function<void(int)> &progress = {});
};

} // namespace lmsc
