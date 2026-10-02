#pragma once

#include "core/AiRecognitionService.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace lmsc {

class AiRecognitionPage : public QWidget {
    Q_OBJECT
public:
    explicit AiRecognitionPage(QWidget *parent = nullptr);
    ~AiRecognitionPage() override;

    void setContext(const QString &audioFile, const QString &title, double bpm,
                    double offsetSeconds, double durationSeconds, bool busy);
    // The caller retains backend ownership. nullptr restores the unavailable
    // backend. Qt connections are removed when either endpoint is destroyed.
    void setService(AiRecognitionService *service);
    bool isRecognizing() const { return !m_pendingContextId.isEmpty(); }

public slots:
    void cancelRecognition();

signals:
    void configureConnectionRequested();
    void analyzeRequested(const lmsc::AiRecognitionRequest &request);
    void cancelRequested(const QString &contextId);

private:
    void startRecognition();
    void refreshControls();
    void showIdleStatus();
    void setStatus(const QString &text, const char *role = "muted");
    bool accepts(const QString &contextId) const;

    UnavailableAiRecognitionService *m_unavailable;
    QPointer<AiRecognitionService> m_service;
    QVector<QMetaObject::Connection> m_connections;
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
    QPushButton *m_cancel;
    QPushButton *m_configure;
    QProgressBar *m_progress;
    QPlainTextEdit *m_result;
};

} // namespace lmsc
