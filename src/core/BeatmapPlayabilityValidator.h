#pragma once

#include "MusicFeatureAnalyzer.h"
#include <QJsonArray>

namespace lmsc {

class BeatmapPlayabilityValidator {
public:
    static bool parseAndValidate(const QJsonObject &json, const GenerationRequest &request,
                                 const MusicAnalysis &analysis, int segmentIndex,
                                 const QString &motifId, const QVector<BeatObject> &previous,
                                 QVector<BeatObject> *objects, QStringList *errors);
    static bool validateObjects(const QVector<BeatObject> &objects, const GenerationRequest &request,
                                const MusicAnalysis &analysis, QStringList *errors);
    static GenerationMetrics metrics(const QVector<BeatObject> &objects,
                                     const TimeMap &timeMap, double activeSeconds);
    static QJsonObject handContext(const QVector<BeatObject> &objects, const TimeMap &timeMap,
                                  double segmentStartBeat);
    static QJsonArray motifReference(const QVector<BeatObject> &objects, double startBeat);
    static double motifDifference(const QVector<BeatObject> &reference, double referenceStart,
                                  const QVector<BeatObject> &objects, double startBeat);
};

} // namespace lmsc
