#pragma once

#include <QObject>

class QThread;
struct RhythmEstimate {
    double bpm = 120;
    double firstBeatSeconds = 0;
    double confidence = 0;
    QString message;
};
Q_DECLARE_METATYPE(RhythmEstimate)

// Energy-onset autocorrelation is a suggestion, never a replacement for manual alignment.
class RhythmAnalyzer : public QObject {
    Q_OBJECT
public:
    explicit RhythmAnalyzer(QObject *parent = nullptr);
    ~RhythmAnalyzer() override;
    bool isBusy() const { return m_thread != nullptr; }
public slots:
    void analyze(const QString &pcmPath, int sampleRate = 44100, int channels = 2);
    void cancel();
signals:
    void analysisFinished(const RhythmEstimate &estimate);
    void errorOccurred(const QString &message);
    void cancelled();
private:
    QThread *m_thread = nullptr;
};
