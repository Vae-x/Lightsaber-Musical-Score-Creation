#include "core/NativeAudioTool.h"
#include "core/WorkerThread.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cmath>
#include <stdexcept>
#include <string>

// Android-only native audio verification. All input is generated in a private
// temporary directory; no user media, game folder or headset song is accessed.
using namespace lmsc;
namespace {
void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toUtf8().constData());
}
void run(const QStringList &arguments) {
    const auto result = NativeAudioTool::run(false, arguments, {}, {}, 20000);
    require(result.exitCode == 0 && !result.timedOut, result.error);
}
QStringList sine(const QString &output, const QString &codec, double duration = 3, bool realtime = false) {
    QStringList arguments{"-hide_banner", "-v", "error", "-nostdin", "-n"};
    if (realtime) arguments << "-re";
    arguments << "-f" << "lavfi" << "-i" << QString("sine=frequency=440:sample_rate=44100:duration=%1").arg(duration)
              << "-ac" << "2" << "-c:a" << codec << output;
    return arguments;
}
QByteArray read(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read synthetic audio: " + path);
    return file.readAll();
}
double duration(const QByteArray &pcm) { return pcm.size() / (44100. * 2 * 2); }
double frequency(const QByteArray &pcm, double begin, double length) {
    const int first = qRound(begin * 44100), frames = qRound(length * 44100);
    require((first + frames) * 4 < pcm.size(), "PCM too short for pitch check");
    int crossings = 0;
    auto sample = [&](int frame) {
        return qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(pcm.constData() + frame * 4));
    };
    for (int frame = first + 1; frame < first + frames; ++frame)
        if (sample(frame - 1) <= 0 && sample(frame) > 0) ++crossings;
    return crossings / length;
}
QByteArray decode(const QString &source, const QString &pcm) {
    run({"-v", "error", "-nostdin", "-n", "-i", source, "-map", "0:a:0", "-ar", "44100", "-ac", "2", "-f", "s16le", pcm});
    return read(pcm);
}
void verify(const QString &root) {
    require(NativeAudioTool::available(), "Android FFmpeg library is unavailable");
    const QDir directory(root);
    const QString ogg = directory.filePath(QStringLiteral("合成音频 '空格'.ogg"));
    run(sine(ogg, "libvorbis"));
    for (const auto &fixture : {qMakePair(QStringLiteral("mp3"), QStringLiteral("libmp3lame")),
                               qMakePair(QStringLiteral("mp4"), QStringLiteral("aac"))}) {
        const QString path = directory.filePath(QStringLiteral("中文文件 '空格'.") + fixture.first);
        run(sine(path, fixture.second));
        const auto result = NativeAudioTool::run(true, {"-v", "error", "-show_format", "-show_streams", "-of", "json", path});
        const auto json = QJsonDocument::fromJson(result.output);
        require(result.exitCode == 0 && json.isObject(), "MP3/MP4 probe JSON: " + result.error);
        const double seconds = json.object().value("format").toObject().value("duration").toString().toDouble();
        require(seconds > 2.9 && seconds < 3.2, "MP3/MP4 probe duration");
    }
    const auto original = decode(ogg, directory.filePath("original.pcm"));
    require(std::abs(duration(original) - 3) < .06 && std::abs(frequency(original, .5, 1) - 440) < 3, "PCM decode and signal");
    const QString cropped = directory.filePath("cropped.ogg");
    run({"-v", "error", "-nostdin", "-n", "-i", ogg, "-ss", "0.5", "-t", "1", "-map", "0:a:0",
         "-c:a", "libvorbis", "-ar", "44100", "-ac", "2", cropped});
    require(std::abs(duration(decode(cropped, directory.filePath("cropped.pcm"))) - 1) < .06, "Vorbis crop duration");
    const QString slow = directory.filePath("slow.pcm");
    run({"-v", "error", "-nostdin", "-n", "-f", "s16le", "-ar", "44100", "-ac", "2", "-i", directory.filePath("original.pcm"),
         "-af", "atempo=0.5", "-f", "s16le", slow});
    const auto slowPcm = read(slow);
    require(std::abs(duration(slowPcm) - 6) < .12 && std::abs(frequency(slowPcm, 1, 1) - 440) < 3, "Half speed retains pitch");
    const QString padded = directory.filePath("padded.ogg");
    run({"-v", "error", "-nostdin", "-n", "-i", ogg, "-ar", "44100", "-af", "aresample=44100,adelay=delays=44100S:all=1",
         "-c:a", "libvorbis", "-ac", "2", padded});
    const auto paddedPcm = decode(padded, directory.filePath("padded.pcm"));
    require(std::abs(duration(paddedPcm) - 4) < .06, "Lead-in duration");
    double energy = 0;
    for (int offset = 0; offset < 44100 * 4 * .9; offset += 2) {
        const double value = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(paddedPcm.constData() + offset));
        energy += value * value;
    }
    require(std::sqrt(energy / (44100 * 2 * .9)) < 2, "Both lead-in channels are silent");

    const QString timeoutPath = directory.filePath("timeout.ogg");
    const auto timeout = NativeAudioTool::run(false, sine(timeoutPath, "libvorbis", 10, true), {}, {}, 200);
    require(timeout.timedOut, "A real native session reaches the deadline");
    const qint64 timeoutSize = QFileInfo(timeoutPath).size();
    QThread::msleep(250);
    require(QFileInfo(timeoutPath).size() == timeoutSize, "No writer remains after timeout returns");

    const auto control = std::make_shared<NativeAudioControl>();
    NativeAudioResult cancelled, independent;
    const QString cancelledPath = directory.filePath("cancelled.ogg"), independentPath = directory.filePath("independent.ogg");
    std::unique_ptr<QThread> cancelledWorker(createWorkerThread([&] { cancelled = NativeAudioTool::run(false, sine(cancelledPath, "libvorbis", 10, true), control); }));
    std::unique_ptr<QThread> independentWorker(createWorkerThread([&] { independent = NativeAudioTool::run(false, sine(independentPath, "libvorbis", 1.5, true)); }));
    cancelledWorker->start(); independentWorker->start();
    QThread::msleep(300); control->cancelled.store(true);
    cancelledWorker->wait(); independentWorker->wait();
    require(cancelled.exitCode != 0 && independent.exitCode == 0, "Cancellation is limited to its own session");
    const qint64 cancelledSize = QFileInfo(cancelledPath).size();
    QThread::msleep(250);
    require(QFileInfo(cancelledPath).size() == cancelledSize, "No writer remains after cancellation returns");
    require(std::abs(duration(decode(independentPath, directory.filePath("independent.pcm"))) - 1.5) < .06, "Independent session completes intact");
    const QString neverStarted = directory.filePath("never-started.ogg");
    const auto preCancelled = NativeAudioTool::run(false, sine(neverStarted, "libvorbis"), control);
    require(preCancelled.exitCode != 0 && !QFileInfo::exists(neverStarted), "Pre-cancelled session never opens output");
    const auto invalid = NativeAudioTool::run(false, {"-v", "error", "-i", ogg, "-af", "invalid_filter_name", directory.filePath("invalid.ogg")});
    require(invalid.exitCode != 0, "Invalid native filter fails without terminating the process");
}
}

class AndroidAudioTest : public QObject {
    Q_OBJECT
private slots:
    void cppExceptionRecovery() {
        // Qt5's Android binaries export their GCC unwinder. A newer libc++ with
        // another unwind implementation can crash here instead of entering the
        // handler; exercise both a libc++ library throw and an application throw.
        bool libraryCaught = false, applicationCaught = false;
        try { (void) std::stoi("synthetic-invalid-integer"); }
        catch (const std::invalid_argument &) { libraryCaught = true; }
        QVERIFY(libraryCaught);
        try { throw std::runtime_error("synthetic Android exception recovery"); }
        catch (const std::runtime_error &error) {
            applicationCaught = std::string(error.what()) == "synthetic Android exception recovery";
        }
        QVERIFY(applicationCaught);
    }

    void nativeAudio() {
        QTemporaryDir directory;
        try {
            require(directory.isValid(), "Cannot create private synthetic fixture directory");
            verify(directory.path());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};

int runAndroidAudioTests(const QString &reportPath) {
    AndroidAudioTest test;
    return QTest::qExec(&test, QStringList{QStringLiteral("android-audio"), QStringLiteral("-o"), reportPath + QStringLiteral(",txt")});
}

#include "AndroidAudioTest.moc"
