#pragma once

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>
#include <QHash>
#include <memory>

class QAudioOutput;
class QThread;
class PcmPlaybackDevice;

struct AudioTrack {
    int index = 0;
    QString codec;
    QString language;
    QString title;
    int channels = 0;
    int sampleRate = 0;
    double duration = 0;
};
struct MediaInfo {
    QString path;
    double duration = 0;
    QVector<AudioTrack> tracks;
};
Q_DECLARE_METATYPE(MediaInfo)

// FFmpeg is an external bundled tool; the application never changes system PATH.
// All positions use seconds in the original (already cropped) song timeline.
class AudioService : public QObject {
    Q_OBJECT
public:
    explicit AudioService(QObject *parent = nullptr);
    ~AudioService() override;
    void setToolsDirectory(const QString &directory);
    QString toolsDirectory() const;
    bool toolsAvailable() const;
    double duration() const { return m_duration; }
    double position() const;
    double playbackSpeed() const { return m_speed; }
    bool isPlaying() const { return m_playing; }
    bool isReady() const { return !m_basePcm.isEmpty(); }
    bool isBusy() const;
    QString pcmCachePath() const { return m_basePcm; }
    QVector<float> waveform() const { return m_waveform; }
    static constexpr int SampleRate = 44100;
    static constexpr int Channels = 2;

public slots:
    void probeMedia(const QString &path);
    // endSeconds <= 0 means until the end; streamIndex is ffprobe's absolute index.
    void convertMedia(const QString &path, int streamIndex, double startSeconds,
                      double endSeconds, const QString &outputOgg);
    void loadAudio(const QString &path, int streamIndex = -1);
    void cancelTask();
    void cancel() { cancelTask(); }
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void setPlaybackSpeed(double speed);
    void setLoop(double startSeconds, double endSeconds, bool enabled);
    void setMetronome(bool enabled, double bpm, double firstBeatSeconds);

signals:
    void mediaProbed(const MediaInfo &info);
    void conversionFinished(const QString &outputOgg);
    void audioReady(double durationSeconds);
    void waveformReady(const QVector<float> &peaks);
    void positionChanged(double seconds);
    void playbackChanged(bool playing);
    void playbackSpeedChanged(double speed);
    void taskProgress(const QString &task, int percent);
    void errorOccurred(const QString &message);
    void cancelled();

private:
    enum class Task { None, Probe, Convert, Decode, Speed };
    void startProcess(Task task, const QString &program, const QStringList &arguments);
    void finishProcess(int exitCode, QProcess::ExitStatus status);
    void failTask(const QString &message);
    void readProcessOutput();
    void buildWaveform(const QString &pcmPath);
    void restartPlayback(double seconds, bool playing);
    void tick();
    QString toolPath(const QString &name) const;
    QString activePcm() const;
    QString m_tools;
    QTemporaryDir m_cache;
    QProcess m_process;
    QTimer m_timer;
    QThread *m_waveThread = nullptr;
    Task m_task = Task::None;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QByteArray m_progressBuffer;
    QString m_source;
    QString m_taskSource;
    QString m_taskOutput;
    QString m_finalOutput;
    QString m_basePcm;
    QHash<QString, QString> m_speedPcm;
    QVector<float> m_waveform;
    double m_duration = 0;
    double m_position = 0;
    double m_playStart = 0;
    double m_speed = 1;
    double m_pendingSpeed = 1;
    double m_taskDuration = 0;
    double m_loopStart = 0;
    double m_loopEnd = 0;
    double m_bpm = 120;
    double m_firstBeat = 0;
    bool m_loop = false;
    bool m_metronome = false;
    bool m_playing = false;
    bool m_resumeAfterTask = false;
    bool m_cancelled = false;
    bool m_shuttingDown = false;
    std::unique_ptr<QAudioOutput> m_audio;
    std::unique_ptr<PcmPlaybackDevice> m_device;
};
