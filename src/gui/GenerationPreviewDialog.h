#pragma once

#include "core/AiGenerationService.h"
#include <QDialog>
#include <QPointer>

class AudioService;
class QLabel;
class QPushButton;
class TimelineView;
class TrackView;

namespace lmsc {
class GenerationPreviewDialog final : public QDialog {
    Q_OBJECT
public:
    explicit GenerationPreviewDialog(const GenerationDraft &draft, int replacedObjectCount,
                                     AudioService *audio, QWidget *parent = nullptr);
    ~GenerationPreviewDialog() override;
    const GenerationDraft &draft() const { return m_draft; }
    void showApplicationError(const QString &message);
signals:
    void applyRequested();
    void regenerateRequested();
private:
    GenerationDraft m_draft;
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
