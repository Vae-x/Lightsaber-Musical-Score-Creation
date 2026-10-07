#include "InfernoSaberGenerationService.h"
#include "BeatmapPlayabilityValidator.h"
#include "LocalChartGenerator.h"
#include "ProjectStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lmsc {
namespace {
using Config = InfernoSaberGenerationService::Config;
void require(bool success, const QString &message) {
    if (!success) throw std::runtime_error(message.toUtf8().constData());
}
QString absolute(const QString &path) { return QFileInfo(path).absoluteFilePath(); }
QString python(const Config &config) { return QDir(config.runtimeDirectory).filePath("env/python.exe"); }
QString source(const Config &config) { return QDir(config.runtimeDirectory).filePath("source"); }
QString model(const GenerationRequest &request) { return request.profile.name == "Hard" ? "easy_15" : "expert_15"; }
QString availability(const Config &config) {
    if (!QFileInfo(python(config)).isFile()) return QStringLiteral("找不到已安装的 InfernoSaber Python 环境，请在设置中选择 E 盘环境目录。");
    if (!QFileInfo(QDir(source(config)).filePath("main.py")).isFile()) return QStringLiteral("模型环境缺少已核对的上游源码。");
    if (!QFileInfo(config.runnerPath).isFile()) return QStringLiteral("程序缺少 InfernoSaber 运行脚本，请重新解压完整便携包。");
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) return QStringLiteral("找不到 Git，无法离线核对固定版本模型源码；请安装 Git 并加入 PATH。");
    for (const QString &name : {QStringLiteral("easy_15"), QStringLiteral("expert_15")})
        if (!QFileInfo(QDir(config.modelCacheDirectory).filePath(name + "/.trial-model-manifest.json")).isFile())
            return QStringLiteral("模型缓存缺少 %1 的校验清单；运行不会自动下载模型。").arg(name);
    if (config.threads < 1 || config.threads > 16 || config.timeoutSeconds < 1 || config.timeoutSeconds > 7200)
        return QStringLiteral("模型线程数或超时时间无效。");
    return {};
}
QString digest(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取音频：%1").arg(path));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const auto data = file.read(1024*1024);
        require(!data.isEmpty() || file.atEnd(), QStringLiteral("读取音频失败。")); hash.addData(data);
    }
    return QString::fromLatin1(hash.result().toHex());
}
QString safeAsset(const QString &root, const QString &relative) {
    require(ProjectStore::safeRelativePath(relative), QStringLiteral("模型输出资源路径不安全。"));
    const auto file = QFileInfo(QDir(root).filePath(relative));
    require(file.isFile() && !file.isSymLink(), QStringLiteral("模型输出缺少资源或包含链接。"));
    const QString prefix = QFileInfo(root).canonicalFilePath() + '/';
    require(file.canonicalFilePath().startsWith(prefix, Qt::CaseInsensitive), QStringLiteral("模型输出资源越过任务目录。"));
    return file.absoluteFilePath();
}
QJsonObject readJson(const QString &path) {
    QJsonObject value; QString error; require(ProjectStore::readJson(path, &value, &error), error); return value;
}
void writeJson(const QString &path, const QJsonObject &value) {
    QString error; require(ProjectStore::writeJson(path, value, &error), error);
}
QJsonArray strings(const QStringList &values) { QJsonArray rows; for (const auto &value : values) rows.append(value); return rows; }
// Mirrors the trial coverage policy: evidence on opposite sides of a quiet
// break cannot be combined to claim a missing continuous musical interval.
QJsonObject coverage(const QVector<BeatObject> &objects, const GenerationRequest &request,
                     const MusicAnalysis &analysis, QStringList *warnings) {
    QVector<double> notes, hits;
    for (const auto &object : objects) notes.append(request.timeMap.beatToSeconds(object.beat));
    std::sort(notes.begin(), notes.end());
    notes.erase(std::unique(notes.begin(), notes.end(), [](double a,double b){return std::abs(a-b)<1e-7;}),notes.end());
    for (const auto &anchor : analysis.anchors)
        if (anchor.kind==MusicAnchorKind::Hit && anchor.strength>=.05 && anchor.confidence>=.35) hits.append(anchor.seconds);
    std::sort(hits.begin(),hits.end());
    int suspicious=0; QJsonArray gaps; double maximumGap=0;
    auto inspect = [&](double from,double to,const QString &kind) {
        const double span=to-from;
        if (kind=="internal") maximumGap=qMax(maximumGap,span);
        const double startBeat=request.timeMap.secondsToBeat(from);
        if (span<qMax(6.0,request.timeMap.beatToSeconds(startBeat+8)-from)-1e-7) return;
        QVector<double> run; bool supported=false;
        auto finish=[&] { if(run.size()>=3 && run.last()-run.first()>=3-1e-7) supported=true; run.clear(); };
        for (double hit:hits) {
            if (hit<from+(kind=="leading"?0:.15) || hit>to-(kind=="trailing"?0:.15)) continue;
            if(!run.isEmpty()) {
                const double beat=request.timeMap.secondsToBeat(run.last());
                if(hit-run.last()>qMax(2.0,request.timeMap.beatToSeconds(beat+4)-run.last())+1e-7) finish();
            }
            run.append(hit);
        }
        finish();
        gaps.append(QJsonObject{{"kind",kind},{"startSeconds",from},{"endSeconds",to},{"suspectedMissingMusic",supported}});
        const QString label=kind=="leading"?QStringLiteral("开头"):kind=="trailing"?QStringLiteral("结尾"):QStringLiteral("中间");
        if(supported) { ++suspicious; warnings->append(QStringLiteral("%1 %2–%3 秒存在连续起音但没有音符，疑似漏谱，请试听检查。").arg(label).arg(from,0,'f',2).arg(to,0,'f',2)); }
        else warnings->append(QStringLiteral("%1保留 %2–%3 秒留白；起音证据不足以判定漏谱，请试听确认。").arg(label).arg(from,0,'f',2).arg(to,0,'f',2));
    };
    if (!notes.isEmpty()) {
        inspect(0,notes.first(),"leading");
        for(int i=1;i<notes.size();++i) inspect(notes[i-1],notes[i],"internal");
        inspect(notes.last(),analysis.durationSeconds,"trailing");
    }
    return {{"coveragePass",!notes.isEmpty()&&suspicious==0},{"suspiciousGapCount",suspicious},
        {"firstNoteSeconds",notes.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(notes.first())},
        {"lastNoteSeconds",notes.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(notes.last())},
        {"maximumInternalGapSeconds",maximumGap},{"longGaps",gaps}};
}

void prepareReference(const GenerationRequest &request, const GenerationDraft &local,
                      const QString &folder, const QString &audioHash) {
    require(QDir().mkpath(folder),QStringLiteral("无法建立本地节奏参考目录。"));
    require(QFile::copy(request.audio.sourcePath,QDir(folder).filePath("song.ogg")),QStringLiteral("无法只读复制原音频。"));
    require(digest(QDir(folder).filePath("song.ogg"))==audioHash,QStringLiteral("原音频复制校验失败。"));
    QJsonArray notes;
    for(const auto &object:local.objects) if(object.kind==ObjectKind::Note) {
        const double seconds=request.timeMap.beatToSeconds(object.beat);
        if(seconds<0 || seconds>=request.audio.durationSeconds) continue;
        notes.append(QJsonObject{{"_time",seconds*request.timeMap.initialBpm()/60.0},{"_lineIndex",object.x},
            {"_lineLayer",object.y},{"_type",object.color},{"_cutDirection",object.direction}});
    }
    const QString chart=request.profile.name+".dat";
    writeJson(QDir(folder).filePath(chart),{{"_version","2.2.0"},{"_notes",notes},{"_obstacles",QJsonArray{}},{"_events",QJsonArray{}}});
    writeJson(QDir(folder).filePath("info.dat"),{{"_version","2.0.0"},{"_songName","本地节奏参考"},
        {"_beatsPerMinute",request.timeMap.initialBpm()},{"_songTimeOffset",0},{"_songFilename","song.ogg"},
        {"_coverImageFilename",""},{"_difficultyBeatmapSets",QJsonArray{QJsonObject{{"_beatmapCharacteristicName","Standard"},
        {"_difficultyBeatmaps",QJsonArray{QJsonObject{{"_difficulty",request.profile.name},{"_difficultyRank",request.profile.rank},
        {"_beatmapFilename",chart},{"_noteJumpMovementSpeed",12},{"_noteJumpStartBeatOffset",0}}}}}}}});
}

GenerationDraft readDraft(const GenerationRequest &request, const MusicAnalysis &analysis,
                          const QString &job, const QString &audioHash) {
    const auto result=readJson(QDir(job).filePath("worker/worker-result.json"));
    require(result.value("complete").toBool(),result.value("error").toString(QStringLiteral("模型没有完成生成。")));
    require(result.value("sourceAudioSha256").toString()==audioHash && result.value("audioTimeShiftSeconds").toDouble(-1)==0,
            QStringLiteral("模型输入音频或时间平移校验失败。"));
    const QString root=absolute(result.value("outputSongDirectory").toString());
    const QString expected=QFileInfo(QDir(job).filePath("worker")).canonicalFilePath()+'/';
    require(QFileInfo(root).canonicalFilePath().startsWith(expected,Qt::CaseInsensitive),QStringLiteral("模型输出不在本任务目录内。"));
    QString infoName;
    for(const auto &name:QDir(root).entryList(QDir::Files)) if(name.compare("info.dat",Qt::CaseInsensitive)==0) infoName=name;
    require(!infoName.isEmpty(),QStringLiteral("模型缺少 Info.dat。"));
    auto info=readJson(safeAsset(root,infoName));
    require(info.value("_version").toString().startsWith("2.") &&
        std::abs(info.value("_beatsPerMinute").toDouble()-request.timeMap.initialBpm())<1e-5 &&
        std::abs(info.value("_songTimeOffset").toDouble())<1e-7,QStringLiteral("模型输出 BPM、偏移或版本不符合基础 v2。"));
    QJsonObject descriptor; int found=0;
    for(const auto &set:info.value("_difficultyBeatmapSets").toArray())
        if(set.toObject().value("_beatmapCharacteristicName").toString()=="Standard")
            for(const auto &value:set.toObject().value("_difficultyBeatmaps").toArray())
                if(value.toObject().value("_difficulty").toString()==request.profile.name) { descriptor=value.toObject(); ++found; }
    require(found==1,QStringLiteral("模型输出目标 Standard 难度不唯一。"));
    auto chart=readJson(safeAsset(root,descriptor.value("_beatmapFilename").toString()));
    require(chart.value("_version").toString().startsWith("2.") && chart.value("_notes").isArray() &&
        chart.value("_obstacles").isArray() && chart.value("_events").isArray(),QStringLiteral("模型输出不是完整基础 v2 谱。"));
    QJsonArray retained; int bombs=0;
    for(const auto &value:chart.value("_notes").toArray()) {
        require(value.isObject(),QStringLiteral("模型音符结构无效。")); const auto note=value.toObject();
        auto integer=[&](const char *name) { const auto v=note.value(name); return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()==std::floor(v.toDouble()); };
        require(integer("_type"),QStringLiteral("模型音符类型无效。"));
        if(note.value("_type").toInt()==3) {++bombs;continue;}
        require((note.value("_type").toInt()==0||note.value("_type").toInt()==1) && note.value("_time").isDouble() &&
            std::isfinite(note.value("_time").toDouble()) && note.value("_time").toDouble()>=0 && integer("_lineIndex") &&
            integer("_lineLayer") && integer("_cutDirection") && note.value("_lineIndex").toInt()>=0 && note.value("_lineIndex").toInt()<=3 &&
            note.value("_lineLayer").toInt()>=0 && note.value("_lineLayer").toInt()<=2 && note.value("_cutDirection").toInt()>=0 &&
            note.value("_cutDirection").toInt()<=8,QStringLiteral("模型音符的时间、位置或切向无效。"));
        retained.append(value);
    }
    const int walls=chart.value("_obstacles").toArray().size();
    chart.insert("_notes",retained); chart.insert("_obstacles",QJsonArray{});
    const QString staged=QDir(job).filePath("validated-song"); require(QDir().mkpath(staged),QStringLiteral("无法建立输出校验目录。"));
    const QString rawAudio=safeAsset(root,info.value("_songFilename").toString());
    require(digest(rawAudio)==audioHash,QStringLiteral("模型输出音频与原 Ogg 字节不一致。"));
    require(QFile::copy(rawAudio,QDir(staged).filePath("song.ogg")),QStringLiteral("无法保留输出校验音频。"));
    QString cover;
    if(!info.value("_coverImageFilename").toString().isEmpty()) {
        const QString rawCover=safeAsset(root,info.value("_coverImageFilename").toString());
        const QString suffix=QFileInfo(rawCover).suffix().toLower();
        require(QStringList{"png","jpg","jpeg"}.contains(suffix),QStringLiteral("模型封面格式不支持。"));
        cover="cover."+suffix; require(QFile::copy(rawCover,QDir(staged).filePath(cover)),QStringLiteral("无法复制模型封面。"));
    }
    info.insert("_songFilename","song.ogg"); info.insert("_coverImageFilename",cover);
    descriptor.insert("_beatmapFilename",request.profile.name+".dat");
    info.insert("_difficultyBeatmapSets",QJsonArray{QJsonObject{{"_beatmapCharacteristicName","Standard"},{"_difficultyBeatmaps",QJsonArray{descriptor}}}});
    writeJson(QDir(staged).filePath("Info.dat"),info); writeJson(QDir(staged).filePath(request.profile.name+".dat"),chart);
    BeatmapDocument document; QString error; require(document.loadSong(staged,&error),error);
    require(document.readOnlyReason().isEmpty(),document.readOnlyReason());
    for(const auto &object:document.objects()) require(!object.isProtected(),object.protectedReason);
    // Only normalize after the original version has passed the core protection checks.
    chart.insert("_version","2.2.0"); writeJson(QDir(staged).filePath(request.profile.name+".dat"),chart);
    require(document.loadSong(staged,&error),error); require(document.readOnlyReason().isEmpty(),document.readOnlyReason());
    GenerationDraft draft; draft.source=request; draft.arrangement=request.arrangement;
    draft.requiresPlayabilityReview=true; draft.playabilityActiveSeconds=analysis.activeSeconds;
    int outside=0,duplicates=0; QSet<QString> occupied;
    for(auto object:document.objects()) {
        require(!object.isProtected()&&object.kind==ObjectKind::Note,QStringLiteral("模型输出包含受保护内容。"));
        const double seconds=document.timeMap().beatToSeconds(object.beat);
        object.beat=request.timeMap.secondsToBeat(seconds);
        if(seconds<0||seconds>=request.audio.durationSeconds||object.beat<0) {++outside;continue;}
        const QString cell=QStringLiteral("%1:%2:%3").arg(qRound64(object.beat*1000000)).arg(object.x).arg(object.y);
        if(occupied.contains(cell)) {++duplicates;continue;}
        occupied.insert(cell); object.id=QUuid::createUuid().toString(QUuid::WithoutBraces); draft.objects.append(object);
    }
    require(!draft.objects.isEmpty(),QStringLiteral("模型没有产生当前音频和首拍范围内的有效音符。"));
    std::stable_sort(draft.objects.begin(),draft.objects.end(),[](const BeatObject &a,const BeatObject &b){return a.beat<b.beat;});
    draft.warnings=analysis.warnings;
    draft.warnings.append(QStringLiteral("InfernoSaber 模型候选需要手动检查动作与头显试玩；未自动写入正式谱。"));
    if(bombs||walls) draft.warnings.append(QStringLiteral("仅音符模式已过滤 %1 个炸弹、%2 面墙，原始模型输出仍保留。").arg(bombs).arg(walls));
    if(outside) draft.warnings.append(QStringLiteral("已忽略 %1 个音频外或工程零拍之前的音符；未平移原音频。").arg(outside));
    if(duplicates) draft.warnings.append(QStringLiteral("已按原始顺序保留同拍同格第一个音符，去重 %1 个；原始输出仍保留。").arg(duplicates));
    QStringList violations; BeatmapPlayabilityValidator::validateLearnedObjects(draft.objects,request,analysis,&violations);
    if(!violations.isEmpty()) {draft.warnings.append(QStringLiteral("完整动作检查未通过："));draft.warnings+=violations;}
    const auto measured=coverage(draft.objects,request,analysis,&draft.warnings);
    draft.metrics=BeatmapPlayabilityValidator::metrics(draft.objects,request.timeMap,analysis.activeSeconds);
    draft.summary=QStringLiteral("InfernoSaber %1：模型动作＋本地补缺节奏，%2 个音符。\n首音 %3 秒；最大中间留白 %4 秒；音乐覆盖%5；动作检查%6。\n原音频和正式谱保持原样，任务日志及原始模型输出：%7")
        .arg(request.profile.name).arg(draft.objects.size()).arg(measured.value("firstNoteSeconds").toDouble(),0,'f',2)
        .arg(measured.value("maximumInternalGapSeconds").toDouble(),0,'f',2)
        .arg(measured.value("coveragePass").toBool()?QStringLiteral("通过"):QStringLiteral("待检查"))
        .arg(violations.isEmpty()?QStringLiteral("通过（仍需试玩）"):QStringLiteral("未通过，需编辑")) .arg(job);
    auto report=measured; report.insert("playabilityPass",violations.isEmpty()); report.insert("actionWarnings",strings(violations));
    report.insert("warnings",strings(draft.warnings)); report.insert("removedBombs",bombs); report.insert("removedWalls",walls);
    report.insert("discardedOutOfRange",outside); report.insert("deduplicatedCells",duplicates);
    report.insert("sourceAudioSha256",audioHash); report.insert("summary",draft.summary);
    writeJson(QDir(job).filePath("validation.json"),report);
    return draft;
}
}

struct InfernoSaberGenerationService::Impl {
    struct Output {
        MusicAnalysis analysis; GenerationDraft draft; QString error,audioHash;
        std::atomic<int> percent{0}; bool success=false;
    };
    InfernoSaberGenerationService *owner; Config config; Status status; GenerationRequest request;
    QThread *thread=nullptr; QProcess *process=nullptr; std::shared_ptr<Output> output;
    QTimer polling,timeout; QByteArray buffered; QFile log; QString job,folderJobId; quint64 epoch=0;
    bool active=false; int lastProgress=-1;
#ifdef Q_OS_WIN
    HANDLE processJob=nullptr;
#endif
    explicit Impl(InfernoSaberGenerationService *service):owner(service) {
        config.runnerPath=QDir(QCoreApplication::applicationDirPath()).filePath("tools/infernosaber/infernosaber_trial.py");
        config.workDirectory=QDir(config.runtimeDirectory).filePath("jobs");
        polling.setInterval(80); timeout.setSingleShot(true);
        QObject::connect(&polling,&QTimer::timeout,owner,[this] {
            if(active&&output&&output->percent.load()!=lastProgress) publish(output->percent.load(),status.stage);
        });
        QObject::connect(&timeout,&QTimer::timeout,owner,[this] {
            if(!active) return; const QString id=request.jobId;
            stop(false); fail(id,QStringLiteral("本地模型超过设定运行时间，已停止；日志与原始输出保留。"));
        });
    }
    ~Impl() {
        active=false; ++epoch; polling.stop();timeout.stop(); killProcess();
        if(process) {QObject::disconnect(process,nullptr,owner,nullptr);process->waitForFinished(3000);delete process;}
        if(thread) {QObject::disconnect(thread,nullptr,owner,nullptr);thread->requestInterruption();thread->wait();delete thread;}
    }
    void killProcess() {
#ifdef Q_OS_WIN
        if(processJob) {TerminateJobObject(processJob,1);CloseHandle(processJob);processJob=nullptr;}
#endif
        if(process&&process->state()!=QProcess::NotRunning) process->kill();
    }
    void publish(int percent,const QString &stage) {
        lastProgress=percent;status.jobId=request.jobId;status.percent=percent;status.stage=status.message=stage;
        emit owner->progress(request.jobId,percent,stage);
    }
    void fail(QString id,const QString &error) {
        active=false;polling.stop();timeout.stop();request={};status.state=Status::Failed;status.jobId=id;
        status.message=error+(job.isEmpty()?QString{}:QStringLiteral("\n任务目录：%1").arg(job));
        emit owner->requestFailed(id,status.message);
    }
    void stop(bool announce) {
        const bool running=active; const QString id=request.jobId;active=false;++epoch;
        polling.stop();timeout.stop();if(thread)thread->requestInterruption();killProcess();request={};status={};
        if(announce&&running)emit owner->cancelled(id);
    }
    void launchThread(bool finishing=false) {
        if(!active||thread||process) return;
        const auto snapshot=request;const QString directory=job;const quint64 token=epoch;
        output=std::make_shared<Output>();const auto result=output;
        if(finishing) result->analysis=cachedAnalysis;
        const auto settings=config;const auto previousHash=audioHash;
        status.stage=finishing?QStringLiteral("正在检查完整谱面、动作与音乐覆盖"):QStringLiteral("正在本地分析音乐并生成节奏基准");
        auto *worker=QThread::create([snapshot,directory,result,settings,finishing,previousHash] {
            try {
                auto cancelled=[] {return QThread::currentThread()->isInterruptionRequested();};
                if(finishing) {
                    require(digest(snapshot.audio.sourcePath)==previousHash,QStringLiteral("原音频在模型运行期间发生变化，拒绝迟到结果。"));
                    result->draft=readDraft(snapshot,result->analysis,directory,previousHash);result->percent=99;
                } else {
                    if(!snapshot.analysisOnly)result->audioHash=digest(snapshot.audio.sourcePath);
                    const bool analyzed=MusicFeatureAnalyzer::analyze(snapshot,&result->analysis,&result->error,cancelled,
                        [result](int value){result->percent=value*20/100;});
                    require(analyzed,result->error);
                    if(cancelled())return;
                    const bool generated=LocalChartGenerator::generate(snapshot,result->analysis,&result->draft,&result->error,cancelled,
                        [result](int value){result->percent=20+value*15/100;});
                    require(generated,result->error);
                    if(cancelled())return;
                    if(!snapshot.analysisOnly) {
                        require(QDir().mkpath(directory),QStringLiteral("无法建立模型任务目录。"));
                        prepareReference(snapshot,result->draft,QDir(directory).filePath("rhythm-reference"),result->audioHash);
                        require(digest(snapshot.audio.sourcePath)==result->audioHash,QStringLiteral("原音频在本地分析期间改变。"));
                    }
                }
                result->success=true;
            } catch(const std::exception &error) {result->error=QString::fromUtf8(error.what());}
        });
        thread=worker;QObject::connect(worker,&QThread::finished,owner,[this,worker,result,token,finishing] {
            thread=nullptr;output.reset();worker->deleteLater();polling.stop();
            if(token!=epoch||!active) {start();return;}
            if(!result->success) {fail(request.jobId,result->error.isEmpty()?QStringLiteral("本地模型准备失败。"):result->error);return;}
            if(finishing||request.analysisOnly) {
                const QString id=request.jobId;
                const QString stage=request.analysisOnly?QStringLiteral("本地音乐分析完成"):QStringLiteral("模型工作草稿已生成，可编辑和试听");
                active=false;timeout.stop();request={};status.state=Status::Completed;status.percent=100;status.jobId=id;
                status.stage=status.message=stage;
                QPointer<InfernoSaberGenerationService> guard(owner);
                emit owner->progress(id,100,stage);
                if(guard&&guard->d->epoch==token)emit guard->draftReady(result->draft);
            } else {cachedAnalysis=result->analysis;audioHash=result->audioHash;launchProcess();}
        });
        polling.start();worker->start();publish(finishing?95:0,status.stage);
    }
    MusicAnalysis cachedAnalysis;QString audioHash;
    void start() {if(active&&!thread&&!process)launchThread();}
    void consume() {
        if(!process)return;const auto bytes=process->readAllStandardOutput();if(log.isOpen())log.write(bytes);
        buffered+=bytes;while(buffered.contains('\n')) {
            const int end=buffered.indexOf('\n');const auto line=buffered.left(end);buffered.remove(0,end+1);
            const auto value=QJsonDocument::fromJson(line).object();if(value.isEmpty()||!active)continue;
            const auto stage=value.value("stage").toString();
            if(stage=="prepare_audio")publish(40,QStringLiteral("正在准备原音频（不平移、不归一化）"));
            else if(stage=="generate")publish(50,QStringLiteral("正在本地模型推理（CPU），请稍候"));
            else if(stage=="pcm_silence")publish(58,QStringLiteral("已按原音频检测真实静音"));
            else if(stage=="rhythm_gap_fill")publish(65,QStringLiteral("已使用本地音乐节奏补缺"));
            else if(stage=="action_input"||stage=="action_padding")publish(70,QStringLiteral("正在生成双手动作与切向"));
            else if(stage=="complete")publish(90,QStringLiteral("模型推理完成，准备完整谱面检查"));
        }
    }
    void launchProcess() {
        if(!active||thread||process)return;
        const quint64 token=epoch;
        process=new QProcess(owner);process->setProcessChannelMode(QProcess::MergedChannels);
        auto environment=QProcessEnvironment::systemEnvironment();
        environment.remove("PYTHONHOME");environment.remove("PYTHONPATH");
        for(const QString &name:QStringList{"HF_HUB_OFFLINE","TRANSFORMERS_OFFLINE","PYTHONUNBUFFERED","PYTHONNOUSERSITE","PYTHONDONTWRITEBYTECODE"})environment.insert(name,"1");
        environment.insert("PYTHONIOENCODING","utf-8");environment.insert("CUDA_VISIBLE_DEVICES","-1");
        environment.insert("PATH",QDir(config.runtimeDirectory).filePath("env")+';'+QDir(config.runtimeDirectory).filePath("env/Library/bin")+';'+
            config.toolsDirectory+';'+environment.value("PATH"));
        process->setProcessEnvironment(environment);process->setWorkingDirectory(job);
#ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments){arguments->flags|=CREATE_NO_WINDOW;});
#endif
        buffered.clear();log.setFileName(QDir(job).filePath("model.log"));log.open(QIODevice::WriteOnly|QIODevice::Append);
        auto *child=process;
        QObject::connect(child,&QProcess::readyReadStandardOutput,owner,[this,token]{if(token==epoch)consume();});
        QObject::connect(child,&QProcess::started,owner,[this,child,token] {
            if(token!=epoch){child->kill();return;}
#ifdef Q_OS_WIN
            HANDLE handle=OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE,FALSE,DWORD(child->processId()));
            HANDLE group=CreateJobObjectW(nullptr,nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if(handle&&group&&SetInformationJobObject(group,JobObjectExtendedLimitInformation,&limits,sizeof(limits))&&AssignProcessToJobObject(group,handle))processJob=group;
            else {
                if(group)CloseHandle(group);
                child->kill();
                fail(request.jobId,QStringLiteral("无法建立本地模型进程组，已停止启动，避免取消后遗留后台任务。"));
            }
            if(handle)CloseHandle(handle);
#endif
        });
        QObject::connect(child,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),owner,[this,child,token](int code,QProcess::ExitStatus exit) {
            if(token==epoch)consume();log.close();process=nullptr;
#ifdef Q_OS_WIN
            if(processJob){CloseHandle(processJob);processJob=nullptr;}
#endif
            child->deleteLater();
            if(token!=epoch||!active){start();return;}
            if(code!=0||exit!=QProcess::NormalExit) {
                QString detail=QStringLiteral("本地模型运行失败（退出码 %1），当前工程和工作草稿未被替换。").arg(code);
                const QString report=QDir(job).filePath("worker/worker-result.json");
                if(QFileInfo(report).isFile()){try{const auto message=readJson(report).value("error").toString();if(!message.isEmpty())detail+='\n'+message;}catch(...) {}}
                fail(request.jobId,detail);return;
            }
            launchThread(true);
        });
        QObject::connect(child,&QProcess::errorOccurred,owner,[this,child,token](QProcess::ProcessError error) {
            if(error!=QProcess::FailedToStart)return;
            log.close();process=nullptr;child->deleteLater();
            if(token==epoch&&active)fail(request.jobId,QStringLiteral("无法启动本地 Python：%1").arg(child->errorString()));else start();
        });
        QStringList arguments{"-u",absolute(config.runnerPath),"worker","--runtime",absolute(config.runtimeDirectory),
            "--model-dir",absolute(QDir(config.modelCacheDirectory).filePath(model(request))),"--model",model(request),
            "--audio",absolute(request.audio.sourcePath),"--job-dir",QDir(job).filePath("worker"),"--difficulty",request.profile.name,
            "--strength",request.profile.name=="Hard"?"2.5":"4.0","--bpm",QString::number(request.timeMap.initialBpm(),'g',17),
            "--seed",QString::number(request.arrangementSeed),"--threads",QString::number(config.threads),
            "--rhythm-reference",QDir(job).filePath("rhythm-reference"),"--notes-only"};
        const QString ffmpeg=QDir(config.toolsDirectory).filePath("ffmpeg.exe");if(QFileInfo(ffmpeg).isFile())arguments<<"--ffmpeg"<<absolute(ffmpeg);
        publish(36,QStringLiteral("正在核对固定版本源码和模型 SHA256（离线）"));
        if(token!=epoch||!active) {
            log.close();if(process==child)process=nullptr;child->deleteLater();start();return;
        }
        child->start(python(config),arguments);
    }
};

InfernoSaberGenerationService::InfernoSaberGenerationService(QObject *parent):AiGenerationService(parent),d(std::make_unique<Impl>(this)) {
    qRegisterMetaType<GenerationRequest>();qRegisterMetaType<GenerationDraft>();
}
InfernoSaberGenerationService::~InfernoSaberGenerationService()=default;
void InfernoSaberGenerationService::setConfig(const Config &config) {d->stop(true);d->config=config;emit availabilityChanged();}
InfernoSaberGenerationService::Config InfernoSaberGenerationService::config() const{return d->config;}
QString InfernoSaberGenerationService::availabilityReason() const{return availability(d->config);}
QString InfernoSaberGenerationService::jobDirectory(const QString &jobId) const {
    return !jobId.isEmpty() && jobId==d->folderJobId && QDir(d->job).exists() ? d->job : QString{};
}
bool InfernoSaberGenerationService::isAvailable() const{return availabilityReason().isEmpty();}
AiGenerationService::Status InfernoSaberGenerationService::status() const{return d->status;}
void InfernoSaberGenerationService::generate(const GenerationRequest &input) {
    QPointer<InfernoSaberGenerationService> guard(this);const auto token=d->epoch+1;d->stop(true);
    if(!guard||guard->d->epoch!=token)return;
    d->job.clear();
    auto request=input;request.profile=DifficultyProfile::forName(input.profile.name);
    if(request.jobId.isEmpty()||!request.audio.isValid()||!std::isfinite(request.audio.durationSeconds)||
       (request.audioRevision&&request.audio.revision&&request.audioRevision!=request.audio.revision)) {
        d->fail(request.jobId,QStringLiteral("歌曲、音频快照或修订无效。"));return;
    }
    if(!request.analysisOnly) {
        if(input.profile.name!="Hard"&&input.profile.name!="Expert") {d->fail(request.jobId,QStringLiteral("现成模型目前只支持 Hard 和 Expert。"));return;}
        if(int(request.allowedTypes)!=int(DirectionalType|DotType)) {d->fail(request.jobId,QStringLiteral("现成模型只支持方向块＋无方向块；炸弹和墙由编辑器手动添加。"));return;}
        if(!request.timeMap.changes().isEmpty()) {d->fail(request.jobId,QStringLiteral("现成模型暂不支持工程变速，请使用本地生成。"));return;}
        if(!isAvailable()){d->fail(request.jobId,availabilityReason());return;}
        QFile audio(request.audio.sourcePath);
        if(!audio.open(QIODevice::ReadOnly)||audio.read(4)!="OggS") {d->fail(request.jobId,QStringLiteral("现成模型需要工程中的原 Ogg 音频；请重新导入音频。"));return;}
    }
    d->request=request;d->active=true;d->status.state=Status::Running;d->status.jobId=request.jobId;
    d->lastProgress=-1;d->audioHash.clear();d->cachedAnalysis={};
    const QString work=d->config.workDirectory.isEmpty()?QDir(d->config.runtimeDirectory).filePath("jobs"):d->config.workDirectory;
    d->job=QDir(absolute(work)).filePath("model-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    d->folderJobId=request.jobId;
    if(!request.analysisOnly)d->timeout.start(d->config.timeoutSeconds*1000);
    d->start();
}
void InfernoSaberGenerationService::cancel(const QString &jobId){if(d->active&&d->request.jobId==jobId)d->stop(true);}
void InfernoSaberGenerationService::discard(const QString &jobId){if(d->status.jobId==jobId)d->stop(false);}

} // namespace lmsc
