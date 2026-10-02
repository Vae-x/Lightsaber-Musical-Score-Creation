#pragma once

#include <QString>
#include <memory>

namespace lmsc {
// A lease keeps the original-speed decoded audio alive across song changes.
struct PcmAudioSnapshot {
    QString path;
    QString sourcePath;
    quint64 revision = 0;
    int sampleRate = 44100;
    int channels = 2;
    double durationSeconds = 0.0;
    std::shared_ptr<void> lease;
    bool isValid() const { return !path.isEmpty() && durationSeconds > 0.0; }
};
}
