#include "RhythmAnalyzer.h"

#include <QFile>
#include <QThread>
#include <QVector>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <memory>

RhythmAnalyzer::RhythmAnalyzer(QObject *parent) : QObject(parent) { qRegisterMetaType<RhythmEstimate>(); }
RhythmAnalyzer::~RhythmAnalyzer() {
    if (m_thread) { m_thread->requestInterruption(); m_thread->wait(); }
}
void RhythmAnalyzer::cancel() { if (m_thread) m_thread->requestInterruption(); }

void RhythmAnalyzer::analyze(const QString &pcmPath, int sampleRate, int channels) {
    if (isBusy()) { emit errorOccurred(tr("节奏分析正在进行。")); return; }
    if (sampleRate < 1000 || channels < 1 || channels > 16) {
        emit errorOccurred(tr("PCM 采样参数不正确。")); return;
    }
    struct Result { RhythmEstimate estimate; QString error; bool cancelled = false; };
    const auto result = std::make_shared<Result>();
    m_thread = QThread::create([pcmPath, sampleRate, channels, result] {
        QFile pcm(pcmPath);
        if (!pcm.open(QIODevice::ReadOnly)) { result->error = QObject::tr("无法读取估拍所需的 PCM 音频。"); return; }
        // At most three minutes are analysed. Input stays on disk; only the 100Hz
        // onset envelope is held in memory, independent of full song length.
        const int hop = qMax(1, sampleRate / 100);
        const double envelopeRate = sampleRate / double(hop);
        const qint64 maxFrames = qMin<qint64>(pcm.size() / (2 * channels), qint64(sampleRate) * 180);
        QVector<double> energy;
        energy.reserve(static_cast<int>(maxFrames / hop + 1));
        qint64 frameIndex = 0;
        double sum = 0;
        int count = 0;
        while (frameIndex < maxFrames && !QThread::currentThread()->isInterruptionRequested()) {
            const QByteArray bytes = pcm.read(qMin<qint64>(65536 / (2 * channels), maxFrames - frameIndex) * (2 * channels));
            if (bytes.isEmpty()) break;
            for (int i = 0; i + 2 * channels <= bytes.size(); i += 2 * channels) {
                double power = 0;
                for (int channel = 0; channel < channels; ++channel) {
                    const double sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(bytes.constData() + i + channel * 2)) / 32768.0;
                    power += sample * sample;
                }
                sum += power / channels; ++count; ++frameIndex;
                if (count == hop) { energy.append(std::sqrt(sum / count)); sum = 0; count = 0; }
            }
        }
        if (QThread::currentThread()->isInterruptionRequested()) { result->cancelled = true; return; }
        if (energy.size() < envelopeRate * 4) {
            result->estimate.message = QObject::tr("音频太短，建议手动设置 BPM 和第一拍。"); return;
        }
        QVector<double> onset(energy.size(), 0);
        double previous = energy[0];
        double onsetPower = 0;
        for (int i = 1; i < energy.size(); ++i) {
            // Positive energy change emphasises attacks; a smoothed baseline
            // reduces sustained vocals without assuming a particular genre.
            onset[i] = qMax(0.0, energy[i] - previous);
            previous = previous * 0.7 + energy[i] * 0.3;
            onsetPower += onset[i] * onset[i];
        }
        if (onsetPower < 1e-6) {
            result->estimate.message = QObject::tr("节拍信号不足，建议手动对拍。"); return;
        }
        const int lowLag = qMax(2, qRound(envelopeRate * 60 / 200));
        const int highLag = qMin(onset.size() / 2, qRound(envelopeRate * 60 / 60));
        QVector<double> scores(highLag + 1, 0);
        int bestLag = lowLag;
        double bestScore = -1, secondScore = 0;
        for (int lag = lowLag; lag <= highLag; ++lag) {
            if (QThread::currentThread()->isInterruptionRequested()) { result->cancelled = true; return; }
            double dot = 0, left = 0, right = 0;
            for (int i = lag; i < onset.size(); ++i) {
                dot += onset[i] * onset[i-lag];
                left += onset[i] * onset[i]; right += onset[i-lag] * onset[i-lag];
            }
            const double bpm = envelopeRate * 60 / lag;
            const double raw = dot / std::sqrt(qMax(1e-20, left * right));
            // Prefer the central practical tempo when equally plausible half-
            // tempo and double-tempo peaks remain. The result is still editable.
            const double score = raw * (1 - 0.06 * std::abs(std::log(bpm / 120)));
            scores[lag] = score;
            if (score > bestScore) { bestScore = score; bestLag = lag; }
        }
        for (int lag = lowLag; lag <= highLag; ++lag)
            if (std::abs(lag - bestLag) > 3) secondScore = qMax(secondScore, scores[lag]);
        double fractionalLag = bestLag;
        if (bestLag > lowLag && bestLag < highLag) {
            const double a = scores[bestLag-1], b = scores[bestLag], c = scores[bestLag+1];
            const double divisor = a - 2 * b + c;
            if (std::abs(divisor) > 1e-9) fractionalLag += qBound(-0.5, 0.5 * (a-c) / divisor, 0.5);
        }
        result->estimate.bpm = envelopeRate * 60 / fractionalLag;
        QVector<double> phases(bestLag, 0);
        for (int i = 0; i < onset.size(); ++i) phases[i % bestLag] += onset[i];
        int phase = 0;
        double strongest = 0;
        for (int i = 0; i < bestLag; ++i) {
            const double value = phases[i] + 0.5 * phases[(i + 1) % bestLag] + 0.5 * phases[(i + bestLag - 1) % bestLag];
            if (value > strongest) { strongest = value; phase = i; }
        }
        result->estimate.firstBeatSeconds = qMax(0.0, phase / envelopeRate - 0.005);
        result->estimate.confidence = qBound(0.0, bestScore * (0.7 + 0.3 * qMax(0.0, bestScore-secondScore)), 1.0);
        result->estimate.message = result->estimate.confidence < 0.35
            ? QObject::tr("节奏估计可信度偏低，请试听拍线并手动校准；可能存在半速/倍速或变速。")
            : QObject::tr("自动估计完成，请试听确认 BPM 和第一拍；可手动调整半速/倍速。");
    });
    QThread *thread = m_thread;
    thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, thread, result] {
        if (m_thread == thread) m_thread = nullptr;
        thread->deleteLater();
        if (result->cancelled) emit cancelled();
        else if (!result->error.isEmpty()) emit errorOccurred(result->error);
        else emit analysisFinished(result->estimate);
    });
    thread->start();
}
