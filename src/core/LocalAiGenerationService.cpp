#include "LocalAiGenerationService.h"
#include "LocalChartGenerator.h"
#include "DiagnosticLog.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <atomic>
#include <cmath>

namespace lmsc {
namespace {
// Metadata is checked on every use, including a cache hit. PCM caches are
// immutable snapshots owned by AudioService; revisions distinguish re-decodes.
QString analysisKey(const GenerationRequest &request) {
    const QFileInfo file(request.audio.path);
    if (!file.isFile() || file.size() <= 0) return {};
    QJsonArray changes;
    for (const auto &change : request.timeMap.changes())
        changes.append(QJsonArray{change.beat, change.bpm});
    const QJsonArray key{file.absoluteFilePath(), QString::number(file.size()),
        QString::number(file.lastModified().toMSecsSinceEpoch()),
        QString::number(request.audioRevision), QString::number(request.audio.revision),
        request.audio.sampleRate, request.audio.channels, request.audio.durationSeconds,
        request.timeMap.initialBpm(), request.timeMap.offsetSeconds(), changes,
        request.profile.subdivision};
    return QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
}
bool validRequest(const GenerationRequest &request) {
    const QStringList difficulties{"Easy", "Normal", "Hard", "Expert", "ExpertPlus"};
    return !request.jobId.isEmpty() && request.audio.isValid()
        && difficulties.contains(request.profile.name)
        && (request.analysisOnly || int(request.allowedTypes) != 0)
        && (int(request.allowedTypes) & ~15) == 0
        && !(request.audioRevision && request.audio.revision && request.audioRevision != request.audio.revision)
        && std::isfinite(request.audio.durationSeconds);
}
}

struct LocalAiGenerationService::Impl {
    struct Result {
        MusicAnalysis analysis;
        GenerationDraft draft;
        QString error, cacheKey;
        bool analysisReady = false, success = false;
        std::atomic<int> percent{0}, stage{0};
    };
    LocalAiGenerationService *owner;
    QThread *worker = nullptr;
    std::shared_ptr<Result> result;
    QTimer timer;
    QElapsedTimer clock;
    GenerationRequest request;
    MusicAnalysis cachedAnalysis;
    QString cachedKey;
    AiGenerationService::Status status;
    quint64 epoch = 0, workerEpoch = 0;
    bool active = false;
    int lastPercent = -1, lastStage = -1;

    explicit Impl(LocalAiGenerationService *service) : owner(service) {
        timer.setParent(owner);
        timer.setInterval(80);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] {
            if (!active || !worker || workerEpoch != epoch || !result) return;
            const int percent = result->percent.load();
            const int stage = result->stage.load();
            if (percent == lastPercent && stage == lastStage) return;
            lastPercent = percent; lastStage = stage;
            publish(percent, stage == 0 ? QStringLiteral("正在本地提取整曲音乐特征")
                                       : QStringLiteral("正在本地选择节奏并搜索动作组合"));
        });
    }
    ~Impl() {
        timer.stop();
        if (worker) {
            QObject::disconnect(worker, nullptr, owner, nullptr);
            worker->requestInterruption();
            worker->wait();
            delete worker;
        }
    }
    void publish(int percent, const QString &text) {
        const QString job = request.jobId, stage = text;
        status.jobId = request.jobId;
        status.percent = percent;
        status.stage = status.message = text;
        emit owner->progress(job, percent, stage);
    }
    void stop(bool announce) {
        const bool wasActive = active;
        const QString job = request.jobId;
        active = false;
        ++epoch;
        timer.stop();
        if (worker) worker->requestInterruption();
        request = {};
        status = {};
        if (announce && wasActive) emit owner->cancelled(job);
    }
    void fail(QString job, QString message) {
        active = false;
        timer.stop();
        request = {};
        status.state = AiGenerationService::Status::Failed;
        status.jobId = job;
        status.message = message;
        DiagnosticLog::instance().record("generation.localFailed",
            {{"jobId", job}, {"stage", "local"}, {"category", "validation"},
             {"elapsedMs", double(clock.isValid() ? clock.elapsed() : 0)}});
        emit owner->requestFailed(job, message);
    }
    void start() {
        if (!active || worker) return;
        const auto snapshot = request;
        const QString key = analysisKey(snapshot);
        if (key.isEmpty()) {
            fail(snapshot.jobId, QStringLiteral("本地分析所需的 PCM 音频不存在或为空，请重新载入歌曲。"));
            return;
        }
        const bool reuse = key == cachedKey && !cachedAnalysis.segments.isEmpty();
        owner->setProperty("localAnalysisCacheHit", reuse);
        const auto cached = reuse ? cachedAnalysis : MusicAnalysis{};
        result = std::make_shared<Result>();
        const auto output = result;
        output->cacheKey = key;
        workerEpoch = epoch;
        const quint64 generation = epoch;
        auto *thread = QThread::create([snapshot, output, cached, reuse] {
            auto cancelled = [] { return QThread::currentThread()->isInterruptionRequested(); };
            if (cancelled()) return;
            if (reuse) {
                output->analysis = cached;
                output->analysisReady = true;
                output->percent = 35;
            } else {
                output->analysisReady = MusicFeatureAnalyzer::analyze(snapshot, &output->analysis,
                    &output->error, cancelled, [output](int percent) { output->percent = percent * 35 / 100; });
            }
            if (!output->analysisReady || cancelled()) return;
            // Reject external changes even if the analysis was reused.
            if (analysisKey(snapshot) != output->cacheKey) {
                output->analysisReady = false;
                output->error = QStringLiteral("音频在本地分析期间发生变化，请重新载入歌曲。");
                return;
            }
            output->stage = 1;
            output->success = LocalChartGenerator::generate(snapshot, output->analysis,
                &output->draft, &output->error, cancelled,
                [output](int percent) { output->percent = 35 + percent * 60 / 100; });
            if (output->success && analysisKey(snapshot) != output->cacheKey) {
                output->success = false;
                output->analysisReady = false;
                output->error = QStringLiteral("音频在本地制谱期间发生变化，请重新载入歌曲。");
            }
        });
        worker = thread;
        thread->setParent(owner);
        QObject::connect(thread, &QThread::finished, owner, [this, thread, output, generation] {
            worker = nullptr;
            result.reset();
            thread->deleteLater();
            if (generation != epoch || !active) {
                // Rapid replacements queue behind the interrupted worker, so
                // repeated clicks never create simultaneous analysis threads.
                start();
                return;
            }
            timer.stop();
            if (output->analysisReady) {
                cachedAnalysis = output->analysis;
                cachedKey = output->cacheKey;
            }
            if (!output->success) {
                fail(request.jobId, output->error.isEmpty()
                    ? QStringLiteral("本地制谱未完成，请检查音频和节拍参数。") : output->error);
                return;
            }
            const QString job = request.jobId;
            active = false;
            request = {};
            status.state = AiGenerationService::Status::Completed;
            status.jobId = job;
            status.percent = 100;
            status.completedSegments = status.totalSegments = output->analysis.segments.size();
            status.stage = status.message = output->draft.source.analysisOnly
                ? QStringLiteral("本地音乐分析完成") : QStringLiteral("本地候选谱已通过校验，可试听预览");
            DiagnosticLog::instance().record("generation.localCompleted",
                {{"jobId", job}, {"stage", "local"}, {"elapsedMs", double(clock.elapsed())},
                 {"segments", status.totalSegments}, {"objects", output->draft.objects.size()}});
            QPointer<LocalAiGenerationService> guard(owner);
            const QString stage = status.stage;
            emit owner->progress(job, 100, stage);
            if (guard && generation == guard->d->epoch) emit guard->draftReady(output->draft);
        });
        lastPercent = lastStage = -1;
        timer.start();
        thread->start();
        publish(reuse ? 35 : 0, reuse ? QStringLiteral("沿用本地音乐分析，开始编排")
                                    : QStringLiteral("正在本地读取原速音频"));
    }
};

LocalAiGenerationService::LocalAiGenerationService(QObject *parent)
    : AiGenerationService(parent), d(std::make_unique<Impl>(this)) {
    qRegisterMetaType<GenerationRequest>();
    qRegisterMetaType<GenerationDraft>();
    setProperty("localAnalysisCacheHit", false);
}
LocalAiGenerationService::~LocalAiGenerationService() = default;
AiGenerationService::Status LocalAiGenerationService::status() const { return d->status; }
void LocalAiGenerationService::generate(const GenerationRequest &input) {
    const GenerationRequest request = input;
    QPointer<LocalAiGenerationService> guard(this);
    const quint64 nextEpoch = d->epoch + 1;
    d->stop(true);
    if (!guard || guard->d->epoch != nextEpoch) return;
    d->clock.start();
    if (!validRequest(request)) {
        d->fail(request.jobId, QStringLiteral("本地制谱请求的歌曲、音频修订、难度或物件选择无效。"));
        return;
    }
    d->request = request;
    // Keep the same conservative shipped profiles as model generation.
    d->request.profile = DifficultyProfile::forName(request.profile.name);
    d->active = true;
    d->status.state = Status::Running;
    d->status.jobId = request.jobId;
    setProperty("localAnalysisCacheHit", false);
    if (d->worker) d->publish(0, QStringLiteral("正在停止上一项本地任务"));
    else d->start();
}
void LocalAiGenerationService::cancel(const QString &jobId) {
    if (d->active && d->request.jobId == jobId) d->stop(true);
}
void LocalAiGenerationService::discard(const QString &jobId) {
    if (d->status.jobId == jobId) d->stop(false);
}

} // namespace lmsc
