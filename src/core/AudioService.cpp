#include "AudioService.h"
#include "WorkerThread.h"
#ifdef Q_OS_ANDROID
#include "NativeAudioTool.h"
#endif

#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int FrameBytes = AudioService::Channels * 2;
constexpr double Pi = 3.14159265358979323846;
QString decimal(double value) { return QString::number(value, 'f', 6); }
QString speedKey(double speed) { return QString::number(speed, 'f', 3); }
}

// QFile remains on disk; the backend pulls bounded blocks. Metronome clicks are
// mixed into that same stream, so scheduling follows the audio sample clock.
class PcmPlaybackDevice : public QIODevice {
public:
    explicit PcmPlaybackDevice(const QString &path) : m_file(path) {
        if (m_file.open(QIODevice::ReadOnly)) open(QIODevice::ReadOnly);
    }
    bool valid() const { return m_file.isOpen(); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override {
        QMutexLocker lock(&m_mutex);
        return (m_loop ? 65536 : qMax<qint64>(0, m_file.size() - m_file.pos()))
               + QIODevice::bytesAvailable();
    }
    void configure(double speed, bool loop, double start, double end,
                   bool metronome, double bpm, double firstBeat) {
        QMutexLocker lock(&m_mutex);
        m_speed = speed; m_loop = loop;
        m_loopStart = qBound<qint64>(0, qRound64(start / speed * AudioService::SampleRate), m_file.size()/FrameBytes);
        m_loopEnd = qBound<qint64>(m_loopStart, qRound64(end / speed * AudioService::SampleRate), m_file.size()/FrameBytes);
        if (m_loopEnd <= m_loopStart) m_loop = false;
        m_metronome = metronome; m_bpm = bpm; m_firstBeat = firstBeat;
    }
    void seekSeconds(double seconds) {
        QMutexLocker lock(&m_mutex);
        m_file.seek(qBound<qint64>(0, qRound64(seconds / m_speed * AudioService::SampleRate) * FrameBytes, m_file.size()));
    }
protected:
    qint64 readData(char *data, qint64 maxSize) override {
        QMutexLocker lock(&m_mutex);
        maxSize -= maxSize % FrameBytes;
        qint64 written = 0;
        while (written < maxSize) {
            qint64 frame = m_file.pos() / FrameBytes;
            if (m_loop && frame >= m_loopEnd) {
                m_file.seek(m_loopStart * FrameBytes);
                frame = m_loopStart;
            }
            qint64 wanted = maxSize - written;
            if (m_loop) wanted = qMin(wanted, (m_loopEnd - frame) * FrameBytes);
            const qint64 count = m_file.read(data + written, wanted);
            if (count <= 0) break;
            if (m_metronome && m_bpm > 0) {
                const double period = 60.0 / m_bpm;
                for (qint64 i = 0; i < count / FrameBytes; ++i) {
                    const double sourceTime = (frame + i) * m_speed / AudioService::SampleRate;
                    const double relative = sourceTime - m_firstBeat;
                    if (relative < 0) continue;
                    const qint64 beat = static_cast<qint64>(std::floor(relative / period));
                    const double elapsed = (relative - beat * period) / m_speed;
                    if (elapsed >= 0.035) continue;
                    const double frequency = beat % 4 == 0 ? 1600 : 1000;
                    const int click = static_cast<int>(6500 * std::exp(-100 * elapsed)
                                      * std::sin(2 * Pi * frequency * elapsed));
                    for (int channel = 0; channel < AudioService::Channels; ++channel) {
                        uchar *sample = reinterpret_cast<uchar *>(data + written + i * FrameBytes + channel * 2);
                        const int current = qFromLittleEndian<qint16>(sample);
                        qToLittleEndian<qint16>(static_cast<qint16>(qBound(-32768, current + click, 32767)), sample);
                    }
                }
            }
            written += count;
        }
        return written;
    }
    qint64 writeData(const char *, qint64) override { return -1; }
private:
    QFile m_file;
    mutable QMutex m_mutex;
    double m_speed = 1;
    double m_bpm = 120;
    double m_firstBeat = 0;
    qint64 m_loopStart = 0, m_loopEnd = 0;
    bool m_loop = false, m_metronome = false;
};

namespace {
struct PcmFileLease {
    QString path;
    std::shared_ptr<QTemporaryDir> directory;
    ~PcmFileLease() { QFile::remove(path); }
};
}

AudioService::AudioService(QObject *parent) : QObject(parent), m_cache(std::make_shared<QTemporaryDir>()) {
    qRegisterMetaType<MediaInfo>();
    qRegisterMetaType<QVector<float>>();
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &AudioService::readProcessOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderr += m_process.readAllStandardError();
        if (m_stderr.size() > 16384) m_stderr = m_stderr.right(16384);
    });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &AudioService::finishProcess);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_task != Task::None)
            failTask(tr("音频工具无法启动：%1").arg(m_process.errorString()));
    });
    m_timer.setInterval(25);
    connect(&m_timer, &QTimer::timeout, this, &AudioService::tick);
}

AudioService::~AudioService() {
    m_shuttingDown = true;
#ifdef Q_OS_ANDROID
    if (m_nativeThread) {
        m_nativeThread->disconnect(this);
        m_nativeControl->cancelled.store(true);
        m_nativeThread->wait();
        if (m_task != Task::Probe && !m_taskOutput.isEmpty()) QFile::remove(m_taskOutput);
    }
#endif
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill(); m_process.waitForFinished(3000);
    }
    if (m_waveThread) {
        m_waveThread->requestInterruption();
        m_waveThread->wait();
    }
    if (m_audio) m_audio->stop();
}

void AudioService::setToolsDirectory(const QString &directory) { m_tools = QDir(directory).absolutePath(); }

QString AudioService::toolsDirectory() const {
    if (!m_tools.isEmpty()) return m_tools;
    QStringList roots;
    QDir directory(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        roots << directory.filePath("tools/ffmpeg") << directory.filePath("third_party/ffmpeg/bin");
        if (!directory.cdUp()) break;
    }
    roots << QDir::current().filePath("third_party/ffmpeg/bin");
    for (const auto &root : roots)
        if (QFileInfo::exists(QDir(root).filePath("ffmpeg.exe")) && QFileInfo::exists(QDir(root).filePath("ffprobe.exe")))
            return QDir(root).absolutePath();
    return {};
}

QString AudioService::toolPath(const QString &name) const {
#ifdef Q_OS_ANDROID
    return name;
#else
    const QString directory = toolsDirectory();
    return directory.isEmpty() ? QString() : QDir(directory).filePath(name + ".exe");
#endif
}
bool AudioService::toolsAvailable() const {
#ifdef Q_OS_ANDROID
    return lmsc::NativeAudioTool::available();
#else
    const QString directory = toolsDirectory();
    return !directory.isEmpty() && QFileInfo::exists(QDir(directory).filePath("ffmpeg.exe"))
           && QFileInfo::exists(QDir(directory).filePath("ffprobe.exe"));
#endif
}
bool AudioService::isBusy() const { return m_task != Task::None || m_waveThread; }

lmsc::PcmAudioSnapshot AudioService::pcmSnapshot() const {
    lmsc::PcmAudioSnapshot snapshot;
    if (!isReady() || isBusy()) return snapshot;
    snapshot.path = m_basePcm;
    snapshot.sourcePath = m_source;
    snapshot.revision = m_pcmRevision;
    snapshot.durationSeconds = m_duration;
    snapshot.sampleRate = SampleRate;
    snapshot.channels = Channels;
    snapshot.lease = m_baseLease;
    return snapshot;
}

void AudioService::startProcess(Task task, const QString &program, const QStringList &arguments) {
    if (isBusy()) { emit errorOccurred(tr("已有音频任务，请等待完成或取消。")); return; }
#ifdef Q_OS_ANDROID
    Q_UNUSED(program)
    if (!toolsAvailable()) {
        emit errorOccurred(tr("APK 缺少可用的 Android 音频组件，请重新安装完整安装包。")); return;
    }
#else
    if (program.isEmpty() || !QFileInfo::exists(program)) {
        emit errorOccurred(tr("缺少随软件提供的音频组件，请重新解压完整便携包。")); return;
    }
#endif
    m_task = task; m_cancelled = false; m_stdout.clear(); m_stderr.clear(); m_progressBuffer.clear();
    emit taskProgress(task == Task::Probe ? tr("检查音轨") : task == Task::Convert ? tr("转换音频") : task == Task::Speed ? tr("准备保持音高的慢放") : tr("解码音频"), -1);
#ifdef Q_OS_ANDROID
    m_nativeControl = std::make_shared<lmsc::NativeAudioControl>();
    const auto control = m_nativeControl;
    const auto result = std::make_shared<lmsc::NativeAudioResult>();
    const double taskDuration = m_taskDuration;
    m_nativeThread = lmsc::createWorkerThread([this, task, arguments, control, result, taskDuration] {
        *result = lmsc::NativeAudioTool::run(task == Task::Probe, arguments, control,
            [this, task, taskDuration](double seconds) {
                if (taskDuration <= 0) return;
                const int percent = qBound(0, static_cast<int>(seconds / taskDuration * 100), 99);
                QMetaObject::invokeMethod(this, [this, task, percent] {
                    if (!m_shuttingDown && !m_cancelled && m_task == task)
                        emit taskProgress(task == Task::Convert ? tr("转换音频") : tr("准备音频"), percent);
                }, Qt::QueuedConnection);
            });
    });
    QThread *thread = m_nativeThread;
    thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, thread, result] {
        m_nativeThread = nullptr;
        m_nativeControl.reset();
        thread->deleteLater();
        if (m_task == Task::Probe) m_stdout = result->output;
        m_stderr = result->error.toUtf8();
        finishProcess(result->timedOut ? -1 : result->exitCode, QProcess::NormalExit);
    });
    thread->start();
#else
    m_process.start(program, arguments);
#endif
}

void AudioService::probeMedia(const QString &path) {
    if (isBusy()) { emit errorOccurred(tr("已有音频任务尚未完成。")); return; }
    m_taskSource = QFileInfo(path).absoluteFilePath();
    startProcess(Task::Probe, toolPath("ffprobe"), {"-v", "error", "-show_format", "-show_streams", "-of", "json", m_taskSource});
}

void AudioService::convertMedia(const QString &path, int streamIndex, double startSeconds,
                                double endSeconds, const QString &outputOgg) {
    if (isBusy()) { emit errorOccurred(tr("已有音频任务尚未完成。")); return; }
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds) || startSeconds < 0 || (endSeconds > 0 && endSeconds <= startSeconds)) {
        emit errorOccurred(tr("裁剪结束时间必须晚于开始时间。")); return;
    }
    m_finalOutput = QFileInfo(outputOgg).absoluteFilePath();
    if (QFileInfo::exists(m_finalOutput)) { emit errorOccurred(tr("输出音频已存在，请选择新的工程目录。")); return; }
    if (!QDir().mkpath(QFileInfo(m_finalOutput).absolutePath())) { emit errorOccurred(tr("无法创建音频输出目录。")); return; }
    m_taskOutput = m_finalOutput + ".import-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".tmp";
    m_taskDuration = endSeconds > 0 ? endSeconds - startSeconds : 0;
    QStringList args = {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", QFileInfo(path).absoluteFilePath(), "-ss", decimal(startSeconds)};
    if (endSeconds > 0) args << "-t" << decimal(endSeconds - startSeconds);
    args << "-map" << (streamIndex < 0 ? "0:a:0" : QString("0:%1").arg(streamIndex))
         << "-vn" << "-sn" << "-dn" << "-map_metadata" << "-1" << "-c:a" << "libvorbis"
         << "-q:a" << "5" << "-ar" << "44100" << "-ac" << "2" << "-f" << "ogg"
         << "-progress" << "pipe:1" << "-nostats" << m_taskOutput;
    startProcess(Task::Convert, toolPath("ffmpeg"), args);
}

void AudioService::loadAudio(const QString &path, int streamIndex) {
    if (isBusy()) { emit errorOccurred(tr("已有音频任务尚未完成。")); return; }
    stop();
    // Temporary audio belongs to this service. Release previous project caches
    // after closing the playback file, rather than retaining every opened song.
    m_baseLease.reset();
    ++m_pcmRevision;
    for (const auto &cache : m_speedPcm) QFile::remove(cache);
    const bool speedChanged = !qFuzzyCompare(m_speed, 1.0);
    m_basePcm.clear(); m_speedPcm.clear(); m_waveform.clear(); m_duration = 0; m_speed = 1; m_loop = false;
    if (speedChanged) emit playbackSpeedChanged(1);
    m_source = QFileInfo(path).absoluteFilePath();
    m_taskOutput = m_cache->filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".pcm");
    m_taskDuration = 0;
    startProcess(Task::Decode, toolPath("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", m_source,
        "-map", streamIndex < 0 ? "0:a:0" : QString("0:%1").arg(streamIndex), "-vn", "-f", "s16le", "-acodec", "pcm_s16le", "-ar", "44100", "-ac", "2", "-progress", "pipe:1", "-nostats", m_taskOutput});
}

void AudioService::readProcessOutput() {
    const QByteArray chunk = m_process.readAllStandardOutput();
    if (m_task == Task::Probe) { m_stdout += chunk; return; }
    m_progressBuffer += chunk;
    int newline;
    while ((newline = m_progressBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_progressBuffer.left(newline).trimmed();
        m_progressBuffer.remove(0, newline + 1);
        if (line.startsWith("out_time_us=") && m_taskDuration > 0) {
            const double seconds = line.mid(12).toDouble() / 1000000;
            emit taskProgress(m_task == Task::Convert ? tr("转换音频") : tr("准备音频"), qBound(0, static_cast<int>(seconds / m_taskDuration * 100), 99));
        }
    }
}

void AudioService::failTask(const QString &message) {
    if (m_task != Task::Probe && !m_taskOutput.isEmpty()) QFile::remove(m_taskOutput);
    m_task = Task::None; m_taskOutput.clear();
    if (!m_shuttingDown) emit errorOccurred(message);
}

void AudioService::finishProcess(int exitCode, QProcess::ExitStatus status) {
    if (m_task == Task::None || m_shuttingDown) return;
    readProcessOutput(); m_stderr += m_process.readAllStandardError();
    if (m_cancelled) {
        if (m_task != Task::Probe) QFile::remove(m_taskOutput);
        m_task = Task::None; emit cancelled(); return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        failTask(tr("音频处理失败：%1").arg(QString::fromUtf8(m_stderr).trimmed().right(3000))); return;
    }
    const Task completed = m_task;
    m_task = Task::None;
    if (completed == Task::Probe) {
        QJsonParseError parseError;
        const auto json = QJsonDocument::fromJson(m_stdout, &parseError);
        if (parseError.error != QJsonParseError::NoError) { emit errorOccurred(tr("音轨信息不是有效 JSON。")); return; }
        MediaInfo info; info.path = m_taskSource;
        info.duration = json.object().value("format").toObject().value("duration").toString().toDouble();
        for (const auto &entry : json.object().value("streams").toArray()) {
            const auto object = entry.toObject();
            if (object.value("codec_type").toString() != "audio") continue;
            AudioTrack track; track.index = object.value("index").toInt();
            track.codec = object.value("codec_name").toString();
            track.channels = object.value("channels").toInt();
            track.sampleRate = object.value("sample_rate").toString().toInt();
            track.duration = object.value("duration").toString().toDouble();
            track.language = object.value("tags").toObject().value("language").toString();
            track.title = object.value("tags").toObject().value("title").toString();
            if (info.duration <= 0) info.duration = qMax(info.duration, track.duration);
            info.tracks.append(track);
        }
        if (info.tracks.isEmpty()) { emit errorOccurred(tr("这个文件没有可用音轨。")); return; }
        emit taskProgress(tr("检查音轨"), 100); emit mediaProbed(info);
    } else if (completed == Task::Convert) {
        if (QFileInfo(m_taskOutput).size() <= 0 || !QFile::rename(m_taskOutput, m_finalOutput)) {
            QFile::remove(m_taskOutput); emit errorOccurred(tr("转换完成，但无法写入工程音频。")); return;
        }
        emit taskProgress(tr("转换音频"), 100); emit conversionFinished(m_finalOutput);
    } else if (completed == Task::Decode) {
        if (QFileInfo(m_taskOutput).size() < FrameBytes) { emit errorOccurred(tr("音频为空，无法试听。")); return; }
        m_basePcm = m_taskOutput;
        auto lease = std::make_shared<PcmFileLease>();
        lease->path = m_basePcm;
        lease->directory = m_cache;
        m_baseLease = lease;
        m_duration = QFileInfo(m_basePcm).size() / double(SampleRate * FrameBytes);
        buildWaveform(m_basePcm);
    } else if (completed == Task::Speed) {
        if (QFileInfo(m_taskOutput).size() < FrameBytes) { emit errorOccurred(tr("慢放缓存为空。")); return; }
        m_speed = m_pendingSpeed; m_speedPcm.insert(speedKey(m_speed), m_taskOutput);
        while (m_speedPcm.size() > 3) {
            auto oldCache = m_speedPcm.begin();
            if (oldCache.key() == speedKey(m_speed)) ++oldCache;
            QFile::remove(oldCache.value()); m_speedPcm.erase(oldCache);
        }
        emit playbackSpeedChanged(m_speed);
        restartPlayback(m_position, m_resumeAfterTask);
        emit taskProgress(tr("准备保持音高的慢放"), 100);
    }
}

void AudioService::buildWaveform(const QString &pcmPath) {
    const double length = m_duration;
    emit taskProgress(tr("分析音频波形"), -1);
    struct WaveResult { QVector<float> peaks; QString error; bool interrupted = false; };
    const auto result = std::make_shared<WaveResult>();
    m_waveThread = lmsc::createWorkerThread([pcmPath, result] {
        QFile file(pcmPath);
        if (!file.open(QIODevice::ReadOnly)) result->error = QObject::tr("无法读取 PCM 波形缓存。");
        else {
            const qint64 frames = file.size() / FrameBytes;
            const int count = static_cast<int>(qMin<qint64>(6000, qMax<qint64>(1, frames / 441)));
            result->peaks.fill(0, count);
            qint64 offset = 0;
            while (!file.atEnd() && !QThread::currentThread()->isInterruptionRequested()) {
                const QByteArray bytes = file.read(65536);
                for (int i = 0; i + FrameBytes <= bytes.size(); i += FrameBytes) {
                    const int bin = qMin(count - 1, static_cast<int>(offset * count / qMax<qint64>(1, frames)));
                    float magnitude = 0;
                    for (int channel = 0; channel < Channels; ++channel)
                        magnitude = qMax(magnitude, std::abs(qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(bytes.constData()+i+channel*2))) / 32768.0f);
                    result->peaks[bin] = qMax(result->peaks[bin], magnitude); ++offset;
                }
            }
        }
        result->interrupted = QThread::currentThread()->isInterruptionRequested();
    });
    QThread *thread = m_waveThread;
    thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, thread, result, length] {
        if (m_waveThread == thread) m_waveThread = nullptr;
        thread->deleteLater();
        if (result->interrupted || !result->error.isEmpty()) {
            m_baseLease.reset(); ++m_pcmRevision;
            m_basePcm.clear(); m_waveform.clear(); m_duration = 0;
            if (result->interrupted) emit cancelled(); else emit errorOccurred(result->error);
            return;
        }
        m_waveform = result->peaks;
        emit waveformReady(m_waveform); emit taskProgress(tr("分析音频波形"), 100); emit audioReady(length);
    });
    thread->start();
}

void AudioService::cancelTask() {
    m_cancelled = true;
#ifdef Q_OS_ANDROID
    if (m_nativeControl) m_nativeControl->cancelled.store(true);
#endif
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
    if (m_waveThread) m_waveThread->requestInterruption();
}

QString AudioService::activePcm() const { return qFuzzyCompare(m_speed, 1.0) ? m_basePcm : m_speedPcm.value(speedKey(m_speed)); }

double AudioService::position() const {
    if (!m_playing || !m_audio) return m_position;
    double value = m_playStart + m_audio->processedUSecs() / 1000000.0 * m_speed;
    if (m_loop && m_loopEnd > m_loopStart && value >= m_loopEnd)
        value = m_loopStart + std::fmod(value - m_loopStart, m_loopEnd - m_loopStart);
    return qBound(0.0, value, m_duration);
}

void AudioService::restartPlayback(double seconds, bool playing) {
    if (m_audio) { m_audio->disconnect(this); m_audio->stop(); }
    m_audio.reset(); m_device.reset(); m_timer.stop(); m_playing = false;
    m_position = qBound(0.0, seconds, m_duration);
    if (m_loop && (m_position < m_loopStart || m_position >= m_loopEnd)) m_position = m_loopStart;
    emit positionChanged(m_position);
    if (!playing || activePcm().isEmpty()) { emit playbackChanged(false); return; }
    QAudioFormat format; format.setSampleRate(SampleRate); format.setChannelCount(Channels);
    format.setSampleSize(16); format.setCodec("audio/pcm"); format.setByteOrder(QAudioFormat::LittleEndian); format.setSampleType(QAudioFormat::SignedInt);
    const auto output = QAudioDeviceInfo::defaultOutputDevice();
    if (output.isNull() || !output.isFormatSupported(format)) {
        emit errorOccurred(tr("当前声音设备不支持 44.1kHz 双声道 PCM，请检查声音输出。")); emit playbackChanged(false); return;
    }
    m_device.reset(new PcmPlaybackDevice(activePcm()));
    if (!m_device->valid()) { emit errorOccurred(tr("PCM 试听缓存无法打开。")); emit playbackChanged(false); return; }
    m_device->configure(m_speed, m_loop, m_loopStart, m_loopEnd, m_metronome, m_bpm, m_firstBeat);
    m_device->seekSeconds(m_position);
    m_audio.reset(new QAudioOutput(output, format));
    m_audio->setBufferSize(SampleRate * FrameBytes / 10);
    connect(m_audio.get(), &QAudioOutput::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::IdleState && m_playing && !m_loop) {
            m_position = m_duration; m_playing = false; m_timer.stop();
            emit positionChanged(m_position); emit playbackChanged(false);
        } else if (state == QAudio::StoppedState && m_audio && m_audio->error() != QAudio::NoError) {
            m_position = position(); m_playing = false; m_timer.stop();
            emit errorOccurred(tr("声音输出中断，请检查声音设备。")); emit playbackChanged(false);
        }
    });
    m_playStart = m_position; m_playing = true; m_audio->start(m_device.get()); m_timer.start(); emit playbackChanged(m_playing);
}

void AudioService::play() {
    if (!isReady() || isBusy()) { emit errorOccurred(tr("音频尚未准备好。")); return; }
    if (m_playing) return;
    restartPlayback(m_position >= m_duration ? 0 : m_position, true);
}
void AudioService::pause() { if (m_playing) restartPlayback(position(), false); }
void AudioService::stop() { restartPlayback(0, false); }
void AudioService::seek(double seconds) { if (std::isfinite(seconds)) restartPlayback(seconds, m_playing); }

void AudioService::setPlaybackSpeed(double speed) {
    if (!std::isfinite(speed) || speed < 0.5 || speed > 2) { emit errorOccurred(tr("播放速度范围为 0.5 至 2。")); return; }
    speed = qRound(speed * 1000) / 1000.0;
    if (qFuzzyCompare(speed, m_speed)) return;
    if (isBusy()) { emit errorOccurred(tr("请等待音频任务完成后修改速度。")); return; }
    if (!isReady()) { emit errorOccurred(tr("请先加载音频。")); return; }
    m_resumeAfterTask = m_playing; const double savedPosition = position(); pause(); m_position = savedPosition;
    if (qFuzzyCompare(speed, 1.0) || m_speedPcm.contains(speedKey(speed))) {
        m_speed = speed; emit playbackSpeedChanged(speed); restartPlayback(savedPosition, m_resumeAfterTask); return;
    }
    m_pendingSpeed = speed; m_taskDuration = m_duration / speed;
    m_taskOutput = m_cache->filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".pcm");
    startProcess(Task::Speed, toolPath("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "s16le", "-ar", "44100", "-ac", "2", "-i", m_basePcm,
        "-af", "atempo=" + decimal(speed), "-f", "s16le", "-acodec", "pcm_s16le", "-progress", "pipe:1", "-nostats", m_taskOutput});
}

void AudioService::setLoop(double startSeconds, double endSeconds, bool enabled) {
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds)) return;
    const double current = position(); const bool playing = m_playing;
    m_loopStart = qBound(0.0, startSeconds, m_duration);
    m_loopEnd = qBound(0.0, endSeconds, m_duration);
    m_loop = enabled && m_loopEnd - m_loopStart >= 0.05;
    restartPlayback(current, playing);
}
void AudioService::setMetronome(bool enabled, double bpm, double firstBeatSeconds) {
    if (!std::isfinite(bpm) || !std::isfinite(firstBeatSeconds) || bpm <= 0) return;
    m_metronome = enabled; m_bpm = bpm; m_firstBeat = firstBeatSeconds;
    if (m_device) m_device->configure(m_speed, m_loop, m_loopStart, m_loopEnd, enabled, bpm, firstBeatSeconds);
}
void AudioService::tick() { emit positionChanged(position()); }
