#include "core/AudioService.h"
#include "core/RhythmAnalyzer.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <cmath>
#include <cstdio>

// Run with a directory containing synthetic clicks.mp3 and two-tracks.mp4.
// The media fixture is generated locally; no user song is part of this test.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) return 2;
    QDir fixtures(QString::fromLocal8Bit(argv[1]));
    AudioService audio;
    audio.setToolsDirectory(QString::fromLocal8Bit(argv[2]));
    RhythmAnalyzer rhythm;
    lmsc::PcmAudioSnapshot previousSnapshot;
    int stage = 0;
    bool failed = false;
    auto fail = [&](const QString &message) {
        std::fprintf(stderr, "FAIL stage %d: %s\n", stage, message.toUtf8().constData());
        failed = true; app.exit(1);
    };
    auto require = [&](bool condition, const char *message) { if (!condition) fail(QString::fromLatin1(message)); return condition; };
    QObject::connect(&audio, &AudioService::errorOccurred, &app, [&](const QString &message) {
        if (stage == 8) {
            stage = 9; audio.probeMedia(fixtures.filePath("video-only.mp4"));
        } else if (stage == 9) {
            stage = 10;
            const QString cancelledOutput = fixtures.filePath("cancelled-output.ogg");
            QFile::remove(cancelledOutput);
            audio.convertMedia(fixtures.filePath("two-tracks.mp4"), 1, 0, 0, cancelledOutput);
            QTimer::singleShot(0, &app, [&] { audio.cancel(); });
        } else if (stage == 11) {
            std::fprintf(stderr, "PASS MP3/MP4 probe, track selection, crop, Ogg, PCM, waveform, atempo, BPM, playback/loop/seek/pause, missing-input, no-audio, cancellation, missing-tools\n");
            app.exit(0);
        } else fail(message);
    });
    QObject::connect(&rhythm, &RhythmAnalyzer::errorOccurred, &app, fail);
    const QString clip = fixtures.filePath("service-clip.ogg");
    QFile::remove(clip);
    QObject::connect(&audio, &AudioService::mediaProbed, &app, [&](const MediaInfo &info) {
        if (stage == 0) {
            if (!require(info.tracks.size() == 1 && info.tracks[0].codec == "mp3" && info.duration > 11, "MP3 probe")) return;
            stage = 1; audio.probeMedia(fixtures.filePath("two-tracks.mp4"));
        } else if (stage == 1) {
            if (!require(info.tracks.size() == 2, "MP4 audio track selection")) return;
            stage = 2; audio.convertMedia(info.path, info.tracks[1].index, 1, 4, clip);
        }
    });
    QObject::connect(&audio, &AudioService::conversionFinished, &app, [&](const QString &path) {
        if (!require(QFileInfo(path).size() > 0, "Ogg conversion output")) return;
        stage = 3; audio.loadAudio(path);
    });
    QObject::connect(&audio, &AudioService::audioReady, &app, [&](double duration) {
        if (!require(!audio.isBusy() && audio.waveform().size() > 0, "waveform worker completion")) return;
        if (stage == 3) {
            if (!require(std::abs(duration - 3) < 0.06, "cropped PCM duration")) return;
            previousSnapshot = audio.pcmSnapshot();
            if (!require(previousSnapshot.isValid() && previousSnapshot.path == audio.pcmCachePath()
                         && bool(previousSnapshot.lease), "original-speed PCM lease")) return;
            stage = 4; audio.setPlaybackSpeed(0.5);
        } else if (stage == 5) {
            const auto current = audio.pcmSnapshot();
            if (!require(current.revision > previousSnapshot.revision
                         && current.path != previousSnapshot.path
                         && QFileInfo::exists(previousSnapshot.path), "song switch preserves analysis PCM lease")) return;
            const QString previousPath = previousSnapshot.path;
            previousSnapshot = {};
            if (!require(!QFileInfo::exists(previousPath), "last analysis lease releases old PCM")) return;
            stage = 6; rhythm.analyze(audio.pcmCachePath());
        }
    });
    QObject::connect(&audio, &AudioService::playbackSpeedChanged, &app, [&](double speed) {
        if (stage == 4) {
            if (!require(std::abs(speed - 0.5) < 0.001, "pitch-preserving half-speed")) return;
            if (!require(audio.pcmSnapshot().path == previousSnapshot.path,
                         "analysis lease stays at original speed")) return;
            // Delay until task completion callback has updated playback state.
            stage = 5; QTimer::singleShot(0, &app, [&] { audio.loadAudio(fixtures.filePath("clicks.mp3")); });
        }
    });
    QObject::connect(&rhythm, &RhythmAnalyzer::analysisFinished, &app, [&](const RhythmEstimate &estimate) {
        std::fprintf(stderr, "Estimate BPM %.3f, first beat %.3f, confidence %.3f\n", estimate.bpm, estimate.firstBeatSeconds, estimate.confidence);
        if (!require(std::abs(estimate.bpm - 120) < 4 && std::abs(estimate.firstBeatSeconds - 0.25) < 0.035, "synthetic 120 BPM and phase")) return;
        stage = 7;
        audio.setLoop(0.2, 0.7, true); audio.setMetronome(true, 120, 0.25); audio.play();
        QTimer::singleShot(900, &app, [&] {
            if (!require(audio.isPlaying() && audio.position() >= 0.2 && audio.position() < 0.7, "streaming loop playback")) return;
            audio.pause(); const double paused = audio.position();
            QTimer::singleShot(100, &app, [&, paused] {
                if (!require(!audio.isPlaying() && std::abs(audio.position()-paused) < 0.001, "pause clock")) return;
                audio.setLoop(0, 0, false); audio.seek(3.0);
                if (!require(std::abs(audio.position()-3) < 0.001, "seek in original song seconds")) return;
                stage = 8; audio.probeMedia(fixtures.filePath("file-does-not-exist.mp3"));
            });
        });
    });
    QObject::connect(&audio, &AudioService::cancelled, &app, [&] {
        if (stage != 10) { fail("unexpected cancellation"); return; }
        if (!require(!QFileInfo::exists(fixtures.filePath("cancelled-output.ogg")), "cancel never commits final output")) return;
        stage = 11; audio.setToolsDirectory(fixtures.filePath("missing-tools"));
        audio.probeMedia(fixtures.filePath("clicks.mp3"));
    });
    QTimer::singleShot(60000, &app, [&] { fail("timeout"); });
    QTimer::singleShot(0, &app, [&] { audio.probeMedia(fixtures.filePath("clicks.mp3")); });
    const int result = app.exec();
    return failed ? 1 : result;
}
