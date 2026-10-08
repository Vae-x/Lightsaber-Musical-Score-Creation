#pragma once

#include <QByteArray>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

namespace lmsc {

// Android executes the bundled FFmpeg libraries in-process. Cancellation belongs
// to one session; it must never cancel another AudioService or an export job.
struct NativeAudioControl {
    std::atomic<bool> cancelled{false};
};

struct NativeAudioResult {
    int exitCode = -1;
    QByteArray output;
    QString error;
    bool timedOut = false;
};

class NativeAudioTool {
public:
    static bool available();
    // Blocking worker-thread API. Arguments stay separate, including Unicode
    // paths and quotes. progress receives the generated audio time in seconds.
    static NativeAudioResult run(bool probe, const QStringList &arguments,
        const std::shared_ptr<NativeAudioControl> &control = {},
        const std::function<void(double)> &progress = {}, int timeoutMs = 1800000);
};

}
