#pragma once

#include "MusicFeatureAnalyzer.h"
#include <QJsonArray>

namespace lmsc {

struct MotifComparison {
    bool comparable = false;
    int referenceNotes = 0, currentNotes = 0, matchedActions = 0;
    double actionDifference = 0, rhythmCoverage = 0, positionDifference = 0, countDifference = 0;
    QJsonArray differences;
    QJsonObject feedback() const;
};

class BeatmapPlayabilityValidator {
public:
    static bool parseAndValidate(const QJsonObject &json, const GenerationRequest &request,
                                 const MusicAnalysis &analysis, int segmentIndex,
                                 const QString &motifId, const QVector<BeatObject> &previous,
                                 QVector<BeatObject> *objects, QStringList *errors);
    static bool validateObjects(const QVector<BeatObject> &objects, const GenerationRequest &request,
                                const MusicAnalysis &analysis, QStringList *errors);
    static bool validateLearnedObjects(const QVector<BeatObject> &objects, const GenerationRequest &request,
                                       const MusicAnalysis &analysis, QStringList *errors);
    static GenerationMetrics metrics(const QVector<BeatObject> &objects,
                                     const TimeMap &timeMap, double activeSeconds);
    static QJsonObject handContext(const QVector<BeatObject> &objects, const TimeMap &timeMap,
                                  double segmentStartBeat);
    static QJsonArray motifReference(const QVector<BeatObject> &objects, double startBeat);
    static MotifComparison compareMotifs(const QVector<BeatObject> &reference, double referenceStart,
                                        const QVector<BeatObject> &objects, double startBeat);
};

} // namespace lmsc
