#pragma once
#include "core/GenerationSource.h"

#include "core/AiGenerationService.h"
#include "core/RefinementTypes.h"
#include <QDialog>
#include <QPointer>
#include <functional>

class AudioService;
class QLabel;
class QPushButton;
class TimelineView;
class TrackView;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QProgressBar;

namespace lmsc {
class AiRefinementService;

class GenerationPreviewDialog final : public QDialog {
    Q_OBJECT
public:
    explicit GenerationPreviewDialog(const GenerationDraft &draft, int replacedObjectCount,
                                     AudioService *audio, QWidget *parent = nullptr, bool addsDifficulty = false);
    ~GenerationPreviewDialog() override;
    const GenerationDraft &draft() const { return m_draft; }
    const GenerationDraft &initialDraft() const { return m_initialDraft; }
    void setRefinementService(AiRefinementService *service);
    void setRefinementResult(const RefinementResult &result);
    void setRefinementSelection(double startSeconds, double endSeconds);
    void setSourceValidation(std::function<bool(const GenerationRequest &)> validation);
    void setDocumentBaseline(bool documentBaseline);
    bool hasRefinementPatch() const { return m_hasRefinement && m_showRefinement; }
    const RefinementResult &refinementResult() const { return m_refinement; }
    bool isRefining() const { return !m_pendingRefinement.generation.jobId.isEmpty(); }
    void invalidateRefinementForConnectionChange();
    void showApplicationError(const QString &message);
public slots:
    void refineChart();
    void resumeRefinement();
    void cancelRefinement();
    void restoreInitialDraft();
signals:
    void applyRequested();
    void regenerateRequested();
    void refinementRequested(const lmsc::RefinementRequest &request);
    void cancelRefinementRequested(const QString &jobId);
    void refinementCompleted(const lmsc::RefinementResult &result);
    void initialDraftRestored();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void displayDraft();
    void refreshRefinementControls();
    bool acceptsRefinement(const RefinementResult &result) const;
    GenerationDraft m_draft;
    GenerationDraft m_initialDraft;
    RefinementResult m_refinement;
    RefinementRequest m_pendingRefinement, m_lastRefinement;
    QPointer<AiRefinementService> m_refinementService;
    QVector<QMetaObject::Connection> m_refinementConnections;
    std::function<bool(const GenerationRequest &)> m_sourceValidation;
    quint64 m_refinementServiceRevision = 0;
    bool m_hasRefinement = false, m_showRefinement = false, m_documentBaseline = false;
    QWidget *m_refinementPanel;
    QWidget *m_refinementScroll;
    QLabel *m_counts, *m_warnings, *m_refinementStats;
    QCheckBox *m_selectedOnly;
    QDoubleSpinBox *m_rangeStart, *m_rangeEnd;
    QComboBox *m_comparison;
    QPushButton *m_refine, *m_resumeRefinement, *m_cancelRefinement, *m_restore, *m_apply;
    QProgressBar *m_refinementProgress;
    QPointer<AudioService> m_audio;
    TimelineView *m_timeline;
    TrackView *m_track;
    QLabel *m_status;
    QPushButton *m_play;
    double m_initialPosition = 0.0;
    bool m_initialPlaying = false;
    bool m_changedPlayback = false;
    bool m_initialLoopEnabled = false;
    double m_initialLoopStart = 0.0, m_initialLoopEnd = 0.0;
};
}
