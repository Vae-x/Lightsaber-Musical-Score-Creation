#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QStringList>
#include <QVector>

namespace lmsc {
struct GenerationRequest;
struct MusicAnalysis;
enum class ActionFamily { Vertical, Diagonal, Horizontal, Staircase, Expansion, QuestionAnswer };
enum class RhythmStrategy { Balanced, StrongBeats, Syncopated, Sparse };
enum class SpaceAmplitude { Compact, Medium, Wide };
enum class ThemeDevelopment { Introduce, Repeat, Vary, Contrast, Build, Close };
struct SectionArrangement {
    QString segmentId, motifId, role;
    bool rest = false;
    double targetNps = 0.0;
    ActionFamily family = ActionFamily::Vertical;
    RhythmStrategy rhythm = RhythmStrategy::Balanced;
    SpaceAmplitude amplitude = SpaceAmplitude::Medium;
    ThemeDevelopment development = ThemeDevelopment::Introduce;
};
struct SongArrangementPlan {
    QString summary, source = QStringLiteral("local"), audioFingerprint, connectionIdentity;
    QVector<SectionArrangement> sections;
    QJsonObject toJson() const;
    QString description() const;
    const SectionArrangement *section(const QString &id) const;
};
class SongArrangementPlanner {
public:
    static SongArrangementPlan localPlan(const GenerationRequest &request, const MusicAnalysis &analysis);
    static QJsonObject outputSchema(const MusicAnalysis &analysis);
    static bool parsePlan(const QJsonObject &json, const GenerationRequest &request,
                          const MusicAnalysis &analysis, SongArrangementPlan *plan, QStringList *errors);
};
}
Q_DECLARE_METATYPE(lmsc::SongArrangementPlan)
