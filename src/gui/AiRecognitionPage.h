#pragma once

#include "core/AiRecognitionService.h"
#include "core/AiGenerationService.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QComboBox;
class QCheckBox;

namespace lmsc {

class AiRecognitionPage : public QWidget {
    Q_OBJECT
public:
    enum GenerationMode { LocalQuick, LanguageModel };
    explicit AiRecognitionPage(QWidget *parent = nullptr);
    ~AiRecognitionPage() override;

    void setContext(const QString &audioFile, const QString &title, double bpm,
                    double offsetSeconds, double durationSeconds, bool busy);
    // The caller retains backend ownership. nullptr restores the unavailable
    // backend. Qt connections are removed when either endpoint is destroyed.
    void setService(AiRecognitionService *service);
    // Configure each caller-owned backend independently; replacing an inactive
    // backend does not interrupt the selected mode or its candidate.
    void setGenerationService(AiGenerationService *service, AiGenerationService *fallback = nullptr);
    void setLocalGenerationService(AiGenerationService *service, AiGenerationService *fallback = nullptr);
    void setGenerationMode(GenerationMode mode);
    GenerationMode generationMode() const { return m_generationMode; }
    void setGenerationContext(const GenerationRequest &context, bool newSong, bool busy);
    void invalidateGeneration();
    void pauseGenerationForConnectionChange();
    void invalidateGenerationForConnectionChange();
    void showGenerationApplied();
    bool isRecognizing() const { return !m_pendingContextId.isEmpty() || !m_pendingGeneration.jobId.isEmpty(); }

public slots:
    void cancelRecognition();
    void generateAgain() { startGeneration(false); }

signals:
    void configureConnectionRequested();
    void analyzeRequested(const lmsc::AiRecognitionRequest &request);
    void cancelRequested(const QString &contextId);
    void generationRequested(const lmsc::GenerationRequest &request);
    void cancelGenerationRequested(const QString &jobId);
    void generationDraftReady(const lmsc::GenerationDraft &draft);
    void generationInvalidated();

private:
    void startRecognition();
    void refreshControls();
    void showIdleStatus();
    void setStatus(const QString &text, const char *role = "muted");
    bool accepts(const QString &contextId) const;
    void startGeneration(bool analysisOnly);
    bool acceptsGeneration(const GenerationRequest &source) const;
    GeneratedTypes selectedTypes() const;
    void activateGenerationService(bool force = false);
    bool usesLocalGeneration() const { return m_generationMode == LocalQuick; }

    UnavailableAiRecognitionService *m_unavailable;
    QPointer<AiRecognitionService> m_service;
    QVector<QMetaObject::Connection> m_connections;
    QPointer<AiGenerationService> m_generationService;
    QPointer<AiGenerationService> m_modelGenerationService;
    QPointer<AiGenerationService> m_generationFallback;
    QPointer<AiGenerationService> m_localGenerationService;
    QPointer<AiGenerationService> m_localGenerationFallback;
    QVector<QMetaObject::Connection> m_generationConnections;
    GenerationRequest m_generationContext;
    GenerationRequest m_pendingGeneration;
    GenerationRequest m_lastGeneration;
    GenerationDraft m_cachedDraft;
    bool m_hasDraft = false;
    GenerationMode m_generationMode = LocalQuick;
    quint64 m_generationServiceRevision = 0;
    bool m_newSong = false;
    bool m_legacyOverride = false;
    bool m_updatingOptions = false;
    QString m_audioFile;
    QString m_title;
    QString m_pendingContextId;
    double m_bpm = 120.0;
    double m_offsetSeconds = 0.0;
    double m_durationSeconds = 0.0;
    quint64 m_sourceRevision = 0;
    quint64 m_serviceRevision = 0;
    bool m_contextBusy = false;
    QLabel *m_songTitle;
    QLabel *m_songDetails;
    QLabel *m_serviceStatus;
    QLabel *m_status;
    QPushButton *m_start;
    QPushButton *m_generate;
    QComboBox *m_mode;
    QComboBox *m_difficulty;
    QCheckBox *m_directional, *m_dots, *m_bombs, *m_walls;
    QPushButton *m_cancel;
    QPushButton *m_resume, *m_preview, *m_logs;
    QPushButton *m_configure;
    QProgressBar *m_progress;
    QPlainTextEdit *m_result;
};

} // namespace lmsc
