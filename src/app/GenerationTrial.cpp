#include "core/AudioService.h"
#include "core/BeatmapDocument.h"
#include "core/BeatmapPlayabilityValidator.h"
#include "core/LocalChartGenerator.h"
#include "core/ProjectStore.h"
#include "core/SongExporter.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointF>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace lmsc;
namespace {
void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toUtf8().constData());
}
QString digest(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取校验文件：%1").arg(path));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), QStringLiteral("无法校验文件：%1").arg(path));
    return QString::fromLatin1(hash.result().toHex());
}
QJsonObject readJson(const QString &path) {
    QJsonObject json; QString error;
    require(ProjectStore::readJson(path, &json, &error), error);
    return json;
}
QJsonObject treeHashes(const QString &folder) {
    QString error; const auto files = ProjectStore::files(folder, &error);
    require(error.isEmpty(), error);
    QJsonObject hashes;
    for (const auto &relative : files) hashes.insert(relative, digest(QDir(folder).filePath(relative)));
    return hashes;
}
QJsonArray strings(const QStringList &values) {
    QJsonArray result; for (const auto &value : values) result.append(value); return result;
}
bool integer(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() == std::floor(value.toDouble());
}
void freshPath(const QString &path, const QString &source = {}) {
    require(!path.isEmpty() && !QFileInfo::exists(path), QStringLiteral("输出必须为尚不存在的路径，未覆盖：%1").arg(path));
    // Resolve existing ancestors before comparing roots: an alias or junction
    // must not turn a seemingly independent output into a source-directory write.
    QFileInfo parent(QFileInfo(path).absoluteFilePath()); QStringList remaining;
    while (!parent.exists()) {
        require(!parent.fileName().isEmpty(),QStringLiteral("输出路径没有可解析的父目录。"));
        remaining.prepend(parent.fileName()); parent=QFileInfo(parent.absolutePath());
    }
    require(parent.isDir() && !parent.canonicalFilePath().isEmpty(),QStringLiteral("输出父路径不是可解析的目录。"));
    const QString absolute=QDir::cleanPath(QDir(parent.canonicalFilePath()).filePath(remaining.join('/')));
    const QString sourceRoot = QFileInfo(source).canonicalFilePath();
    require(sourceRoot.isEmpty() || (absolute.compare(sourceRoot, Qt::CaseInsensitive) != 0
        && !absolute.startsWith(sourceRoot + '/', Qt::CaseInsensitive)), QStringLiteral("输出不能位于原工程或原歌曲目录中。"));
}
void decode(AudioService &audio, const QString &path) {
    QEventLoop loop; QString error; bool ready = false;
    QTimer timeout; timeout.setSingleShot(true);
    auto readyConnection = QObject::connect(&audio, &AudioService::audioReady, &loop, [&](double) { ready = true; loop.quit(); });
    auto errorConnection = QObject::connect(&audio, &AudioService::errorOccurred, &loop, [&](const QString &message) { error = message; loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { error = QStringLiteral("音频解码超过 180 秒，已停止。"); audio.cancelTask(); loop.quit(); });
    timeout.start(180000); audio.loadAudio(path); if (!ready && error.isEmpty()) loop.exec();
    QObject::disconnect(readyConnection); QObject::disconnect(errorConnection);
    require(ready && audio.pcmSnapshot().isValid(), error.isEmpty() ? QStringLiteral("未获得有效音频。") : error);
}
GenerationRequest requestFor(const BeatmapDocument &document, const AudioService &audio, const QString &difficulty, quint32 seed) {
    GenerationRequest request; request.jobId = QStringLiteral("offline-trial"); request.documentId = QStringLiteral("read-only-source");
    request.documentRevision = document.revision(); request.audio = audio.pcmSnapshot(); request.audioRevision = request.audio.revision;
    request.timeMap = document.timeMap(); request.profile = DifficultyProfile::forName(difficulty);
    request.allowedTypes = DirectionalType | DotType; request.arrangementSeed = seed; return request;
}
QPointF vectorFor(int direction) {
    const double diagonal = std::sqrt(.5);
    const QPointF vectors[] = {{0,1},{0,-1},{-1,0},{1,0},{-diagonal,diagonal},{diagonal,diagonal},{-diagonal,-diagonal},{diagonal,-diagonal},{0,0}};
    return direction >= 0 && direction <= 8 ? vectors[direction] : QPointF{};
}
QString action(const BeatObject &note) {
    return QStringLiteral("%1:%2:%3:%4").arg(note.color).arg(note.x).arg(note.y).arg(note.direction);
}
QJsonObject inspect(const QVector<BeatObject> &objects, const GenerationRequest &request,
                    const MusicAnalysis &analysis, QStringList *errors) {
    BeatmapPlayabilityValidator::validateObjects(objects, request, analysis, errors);
    QVector<BeatObject> notes;
    for (const auto &object : objects) if (object.kind == ObjectKind::Note) notes.append(object);
    std::stable_sort(notes.begin(), notes.end(), [](const BeatObject &a, const BeatObject &b) { return a.beat < b.beat; });
    const auto measured = BeatmapPlayabilityValidator::metrics(objects, request.timeMap, analysis.activeSeconds);
    QVector<int> directions(9), grid(12), hands(2); QVector<QString> tokens;
    const BeatObject *prior[2] = {nullptr, nullptr};
    int backstrokes = 0, fastConnections = 0, shortHandGaps = 0, crossHalf = 0, loops16 = 0, longestLoop = 0;
    for (const auto &note : notes) {
        if (note.direction >= 0 && note.direction <= 8) ++directions[note.direction];
        if (note.x >= 0 && note.x < 4 && note.y >= 0 && note.y < 3) ++grid[note.y * 4 + note.x];
        tokens.append(action(note));
        if (note.color < 0 || note.color > 1) continue;
        ++hands[note.color]; if ((note.color == 0 && note.x > 1) || (note.color == 1 && note.x < 2)) ++crossHalf;
        const auto *previous = prior[note.color];
        if (previous) {
            const double dt = request.timeMap.beatToSeconds(note.beat) - request.timeMap.beatToSeconds(previous->beat);
            if (dt < request.profile.minSameHandGapSeconds - 1e-7) ++shortHandGaps;
            const auto movement = QPointF(note.x, note.y) - vectorFor(note.direction) * .35
                - (QPointF(previous->x, previous->y) + vectorFor(previous->direction) * .35);
            if (std::hypot(movement.x(), movement.y()) > qMax(0.0, dt) * request.profile.maxConnectionSpeed + .15 + 1e-7) ++fastConnections;
            if (dt <= 1.0 + 1e-7 && previous->direction != 8 && note.direction != 8
                && QPointF::dotProduct(vectorFor(previous->direction), vectorFor(note.direction)) > .1) {
                ++backstrokes;
                if (errors->size() < 40) errors->append(QStringLiteral("第 %1 拍%2手在一秒内连续同向回刀，需要不可见回位。")
                    .arg(note.beat).arg(note.color == 0 ? QStringLiteral("左") : QStringLiteral("右")));
            }
        }
        prior[note.color] = &note;
    }
    for (int period : {4, 8, 16}) {
        int streak = 0;
        for (int i = period; i < tokens.size(); ++i) {
            streak = tokens[i] == tokens[i-period] ? streak+1 : 0;
            if (streak >= period) {
                longestLoop = qMax(longestLoop, streak+period);
                if (period == 16 && streak == period) ++loops16;
            }
        }
    }
    int phraseRun = 0, longestPhraseRun = 0; QString previousPhrase;
    for (const auto &phrase : analysis.phrases) {
        QString signature;
        for (const auto &note : notes) if (note.beat >= phrase.startBeat && note.beat < phrase.endBeat)
            signature += QString::number(qRound64((note.beat-phrase.startBeat)*1000000)) + ':' + action(note) + ';';
        phraseRun = !signature.isEmpty() && signature == previousPhrase ? phraseRun+1 : signature.isEmpty() ? 0 : 1;
        longestPhraseRun = qMax(longestPhraseRun, phraseRun); previousPhrase = signature;
    }
    auto counts = [](const QVector<int> &values) { QJsonArray result; for (int count : values) result.append(count); return result; };
    return {{"notes",notes.size()},{"directional",measured.directional},{"dots",measured.dots},
        {"bombs",measured.bombs},{"walls",measured.walls},{"averageNps",measured.averageNps},{"peakNps",measured.peakNps},
        {"durationSeconds",analysis.durationSeconds},{"activeSeconds",analysis.activeSeconds},
        {"directionCounts",counts(directions)},{"gridCounts",counts(grid)},{"handCounts",counts(hands)},
        {"backstrokeViolations",backstrokes},{"connectionSpeedViolations",fastConnections},{"sameHandGapViolations",shortHandGaps},
        {"crossHalfViolations",crossHalf},{"fixed16LoopOccurrences",loops16},{"longestRepeatedActionRun",longestLoop},
        {"longestRepeatedPhraseRun",longestPhraseRun}};
}
void inspectCoverage(const QVector<BeatObject> &objects, const GenerationRequest &request,
                     const MusicAnalysis &analysis, QJsonObject *report) {
    constexpr double minimumGapSeconds=6.0, minimumGapBeats=8.0;
    constexpr double minimumHitStrength=.05, minimumHitConfidence=.35, minimumHitSpanSeconds=3.0;
    constexpr int minimumHitCount=3;
    constexpr double edgeToleranceSeconds=.15;
    QVector<double> notes;
    for (const auto &object : objects) if (object.kind==ObjectKind::Note) {
        const double seconds=request.timeMap.beatToSeconds(object.beat);
        if (std::isfinite(seconds) && seconds>=0 && seconds<analysis.durationSeconds) notes.append(seconds);
    }
    std::sort(notes.begin(),notes.end());
    notes.erase(std::unique(notes.begin(),notes.end(),[](double a,double b) { return std::abs(a-b)<1e-7; }),notes.end());
    QVector<double> hits;
    for (const auto &anchor : analysis.anchors)
        if (anchor.kind==MusicAnchorKind::Hit && anchor.strength>=minimumHitStrength && anchor.confidence>=minimumHitConfidence)
            hits.append(anchor.seconds);
    std::sort(hits.begin(),hits.end());
    QStringList warnings; int suspicious=0;
    auto gap = [&](double start,double end,const QString &kind) {
        const double startBeat=request.timeMap.secondsToBeat(start);
        const double duration=qMax(0.0,end-start);
        const double threshold=qMax(minimumGapSeconds,request.timeMap.beatToSeconds(startBeat+minimumGapBeats)-start);
        const bool longGap=duration>=threshold-1e-7;
        const double evidenceStart=start+(kind=="leading" ? 0.0 : edgeToleranceSeconds);
        const double evidenceEnd=end-(kind=="trailing" ? 0.0 : edgeToleranceSeconds);
        QJsonArray runs; QVector<double> run; int hitCount=0, bestCount=0; double bestSpan=0.0;
        auto finishRun = [&] {
            if (run.isEmpty()) return;
            const double span=run.last()-run.first();
            const bool supported=run.size()>=minimumHitCount && span>=minimumHitSpanSeconds-1e-7;
            runs.append(QJsonObject{{"firstHitSeconds",run.first()},{"lastHitSeconds",run.last()},
                                   {"hitCount",run.size()},{"hitSpanSeconds",span},{"supportsMissingMusic",supported}});
            if (supported && span>bestSpan) { bestSpan=span; bestCount=run.size(); }
            run.clear();
        };
        for (double hit : hits) {
            if (hit<evidenceStart || hit>evidenceEnd) continue;
            ++hitCount;
            if (!run.isEmpty()) {
                const double priorBeat=request.timeMap.secondsToBeat(run.last());
                // Evidence on opposite sides of a real quiet break must not
                // combine into a fictitious continuous musical interval.
                const double mergeGap=qMax(2.0,request.timeMap.beatToSeconds(priorBeat+4.0)-run.last());
                if (hit-run.last()>mergeGap+1e-7) finishRun();
            }
            run.append(hit);
        }
        finishRun();
        const bool missing=longGap && bestCount>=minimumHitCount;
        if (missing) {
            ++suspicious;
            const QString label=kind=="leading" ? QStringLiteral("开头") : kind=="trailing" ? QStringLiteral("结尾") : QStringLiteral("中间");
            warnings.append(QStringLiteral("%1 %2–%3 秒没有音符，但有连续音乐起音证据（%4 个，跨度 %5 秒），请检查漏谱。")
                .arg(label).arg(start,0,'f',1).arg(end,0,'f',1).arg(bestCount).arg(bestSpan,0,'f',1));
        }
        return QJsonObject{{"kind",kind},{"startSeconds",start},{"endSeconds",end},{"durationSeconds",duration},
            {"gapBeats",request.timeMap.secondsToBeat(end)-startBeat},{"minimumLongGapSeconds",threshold},
            {"longGap",longGap},{"validHitCount",hitCount},{"hitEvidenceRuns",runs},
            {"supportedHitCount",bestCount},{"supportedHitSpanSeconds",bestSpan},{"suspectedMissingMusic",missing}};
    };
    report->insert("firstNoteSeconds",notes.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(notes.first()));
    report->insert("lastNoteSeconds",notes.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(notes.last()));
    const auto leading=gap(0.0,notes.isEmpty() ? analysis.durationSeconds : notes.first(),"leading");
    const auto trailing=notes.isEmpty() ? QJsonObject{} : gap(notes.last(),analysis.durationSeconds,"trailing");
    QJsonArray internal;
    for (int i=1;i<notes.size();++i) {
        const double start=notes[i-1],end=notes[i];
        if (end-start<minimumGapSeconds-1e-7) continue;
        const auto interval=gap(start,end,"internal");
        if (interval.value("longGap").toBool()) internal.append(interval);
    }
    if (notes.isEmpty()) warnings.prepend(QStringLiteral("整曲没有有效音符，不能作为完整试用谱。"));
    report->insert("coveragePass",!notes.isEmpty() && suspicious==0);
    report->insert("coverageWarnings",strings(warnings));
    report->insert("coverage",QJsonObject{{"thresholds",QJsonObject{{"minimumGapSeconds",minimumGapSeconds},
        {"minimumGapBeats",minimumGapBeats},{"minimumHitCount",minimumHitCount},{"minimumHitStrength",minimumHitStrength},
        {"minimumHitConfidence",minimumHitConfidence},{"minimumHitSpanSeconds",minimumHitSpanSeconds},
        {"minimumHitRunMergeGapSeconds",2.0},{"hitRunMergeGapBeats",4.0},{"edgeToleranceSeconds",edgeToleranceSeconds}}},
        {"leadingGap",leading},{"trailingGap",trailing},{"longInternalGaps",internal},{"suspiciousGapCount",suspicious},
        {"policy",QStringLiteral("按起音证据提示疑似漏谱；安静休止不判失败，持续无起音的音乐可能无法检出。")}});
    auto combinedWarnings=report->value("warnings").toArray();
    for (const auto &warning : warnings) combinedWarnings.append(warning);
    report->insert("warnings",combinedWarnings);
}
QString rawAsset(const QString &folder, const QString &relative);
QJsonObject sourceInfo(const BeatmapDocument &document) {
    QJsonArray changes;
    for (const auto &change : document.timeMap().changes()) changes.append(QJsonObject{{"beat",change.beat},{"bpm",change.bpm}});
    QJsonObject hashes{{"audioSha256",digest(document.audioPath())}};
    if (!document.coverPath().isEmpty()) hashes.insert("coverSha256",digest(document.coverPath()));
    if (QFileInfo(document.projectPath()).isFile()) hashes.insert("projectSha256",digest(document.projectPath()));
    // loadProject owns a temporary immutable resource snapshot. A subprocess
    // consumer needs the verified persisted paths after this process exits.
    QString audioPath=document.audioPath(),coverPath=document.coverPath();
    if (QFileInfo(document.projectPath()).isFile()) {
        const auto manifest=readJson(document.projectPath()); const auto assets=manifest.value("assets").toString();
        require(ProjectStore::safeRelativePath(assets),QStringLiteral("工程持久资源路径无效。"));
        const auto root=QDir(QFileInfo(document.projectPath()).absolutePath()).filePath(assets);
        const auto info=manifest.value("info").toObject();
        audioPath=rawAsset(root,info.value("_songFilename").toString());
        require(digest(audioPath)==hashes.value("audioSha256").toString(),QStringLiteral("工程音频在读取后改变。"));
        if (!coverPath.isEmpty()) {
            coverPath=rawAsset(root,info.value("_coverImageFilename").toString());
            require(digest(coverPath)==hashes.value("coverSha256").toString(),QStringLiteral("工程封面在读取后改变。"));
        }
    }
    return {{"project",document.projectPath()},{"title",document.title()},{"songName",document.title()},
        {"audioPath",audioPath},{"coverPath",coverPath},
        {"baseBpm",document.timeMap().baseBpm()},{"bpm",document.timeMap().baseBpm()},
        {"firstBeatSeconds",document.timeMap().firstBeatSeconds()},{"changes",changes},{"sourceHashes",hashes}};
}
QString rawAsset(const QString &folder, const QString &relative) {
    require(ProjectStore::safeRelativePath(relative), QStringLiteral("歌曲资源路径无效：%1").arg(relative));
    const auto path = QDir(folder).filePath(relative);
    require(QFileInfo(path).isFile() && !QFileInfo(path).isSymLink(), QStringLiteral("歌曲资源丢失或为链接：%1").arg(relative));
    return path;
}
void copyAsset(const QString &source, const QString &target) {
    require(QFile::copy(source, target), QStringLiteral("无法复制试用资源：%1").arg(source));
    require(digest(source) == digest(target), QStringLiteral("试用资源校验不一致。"));
}
void validateRawNotes(const QJsonArray &notes) {
    for (const auto &value : notes) {
        require(value.isObject(), QStringLiteral("音符必须为对象。")); const auto note = value.toObject();
        require(integer(note.value("_type")), QStringLiteral("音符类型必须为整数。"));
        const int type = note.value("_type").toInt(); if (type == 3) continue;
        require(type == 0 || type == 1, QStringLiteral("试用仅支持基础双手音符。"));
        require(note.value("_time").isDouble() && std::isfinite(note.value("_time").toDouble()) && note.value("_time").toDouble() >= 0,
                QStringLiteral("音符拍时间无效。"));
        for (const auto &field : {QStringLiteral("_lineIndex"),QStringLiteral("_lineLayer"),QStringLiteral("_cutDirection")})
            require(integer(note.value(field)), QStringLiteral("音符格位和切向必须为整数。"));
        require(note.value("_lineIndex").toInt() >= 0 && note.value("_lineIndex").toInt() <= 3
            && note.value("_lineLayer").toInt() >= 0 && note.value("_lineLayer").toInt() <= 2
            && note.value("_cutDirection").toInt() >= 0 && note.value("_cutDirection").toInt() <= 8, QStringLiteral("音符坐标或方向超出基础格式。"));
        for (const auto &key : {QStringLiteral("_customData"),QStringLiteral("customData")})
            require(!note.contains(key) || (note.value(key).isObject() && note.value(key).toObject().isEmpty()), QStringLiteral("试用不支持音符模组数据。"));
    }
}
void checkSong(const QString &source, const QString &difficulty, const QString &output, AudioService &audio, QJsonObject *report) {
    const auto before = treeHashes(source); freshPath(output, source);
    QString infoFile;
    for (const auto &name : QDir(source).entryList(QDir::Files)) if (name.compare("Info.dat",Qt::CaseInsensitive)==0) infoFile=name;
    require(!infoFile.isEmpty(), QStringLiteral("模型输出缺少 Info.dat。"));
    auto info = readJson(QDir(source).filePath(infoFile));
    require(info.value("_version").toString().startsWith("2."), QStringLiteral("试用仅支持 v2 歌曲信息。"));
    QVector<QJsonObject> choices;
    for (const auto &set : info.value("_difficultyBeatmapSets").toArray()) if (set.toObject().value("_beatmapCharacteristicName").toString()=="Standard")
        for (const auto &map : set.toObject().value("_difficultyBeatmaps").toArray()) if (map.isObject()) choices.append(map.toObject());
    require(!choices.isEmpty(), QStringLiteral("模型输出没有 Standard 难度。"));
    QJsonObject descriptor;
    for (const auto &choice : choices) if (choice.value("_difficulty").toString()==difficulty) descriptor=choice;
    if (descriptor.isEmpty() && choices.size()==1) descriptor=choices.first();
    require(!descriptor.isEmpty(), QStringLiteral("存在多个 Standard 难度，不能猜测目标谱。"));
    auto map = readJson(rawAsset(source,descriptor.value("_beatmapFilename").toString()));
    require(map.value("_version").toString().startsWith("2.") && map.value("_notes").isArray() && map.value("_obstacles").isArray()
        && map.value("_events").isArray(), QStringLiteral("模型谱缺少有效的基础 v2 数组。"));
    validateRawNotes(map.value("_notes").toArray());
    QJsonArray retained; int bombs=0;
    for (const auto &note : map.value("_notes").toArray()) {
        if (note.toObject().value("_type").toInt()==3) ++bombs; else retained.append(note);
    }
    const int walls=map.value("_obstacles").toArray().size();
    map.insert("_notes",retained); map.insert("_obstacles",QJsonArray{});
    const QFileInfo target(output); require(QDir().mkpath(target.absolutePath()), QStringLiteral("无法创建试用输出父目录。"));
    QTemporaryDir staging(QDir(target.absolutePath()).filePath(".lmsc-trial-XXXXXX")); require(staging.isValid(), QStringLiteral("无法创建试用暂存目录。"));
    const QString staged=QDir(staging.path()).filePath("song"); require(QDir().mkpath(staged),QStringLiteral("无法创建试用副本。"));
    const auto audioPath=rawAsset(source,info.value("_songFilename").toString()); copyAsset(audioPath,QDir(staged).filePath("song.ogg"));
    QString coverName; const auto rawCover=info.value("_coverImageFilename").toString();
    if (!rawCover.isEmpty()) {
        const auto coverPath=rawAsset(source,rawCover); const auto suffix=QFileInfo(coverPath).suffix().toLower();
        require(QStringList{"png","jpg","jpeg"}.contains(suffix),QStringLiteral("封面不是 PNG/JPEG。"));
        coverName="cover."+suffix; copyAsset(coverPath,QDir(staged).filePath(coverName));
    }
    info.insert("_songFilename","song.ogg"); info.insert("_coverImageFilename",coverName);
    descriptor.insert("_difficulty",difficulty); descriptor.insert("_difficultyRank",DifficultyProfile::forName(difficulty).rank);
    descriptor.insert("_beatmapFilename",difficulty+".dat");
    info.insert("_difficultyBeatmapSets",QJsonArray{QJsonObject{{"_beatmapCharacteristicName","Standard"},{"_difficultyBeatmaps",QJsonArray{descriptor}}}});
    QString error;
    require(ProjectStore::writeJson(QDir(staged).filePath("Info.dat"),info,&error),error);
    require(ProjectStore::writeJson(QDir(staged).filePath(difficulty+".dat"),map,&error),error);
    BeatmapDocument document; require(document.loadSong(staged,&error),error);
    require(document.readOnlyReason().isEmpty(),document.readOnlyReason());
    for (const auto &object : document.objects()) require(!object.isProtected(),object.protectedReason);
    // Check the original version before changing the independent copy's marker.
    // Version-dependent timing/light protection must survive normalization.
    const auto sourceVersion=map.value("_version").toString();
    if (sourceVersion!=QStringLiteral("2.2.0")) {
        map.insert("_version",QStringLiteral("2.2.0"));
        require(ProjectStore::writeJson(QDir(staged).filePath(difficulty+".dat"),map,&error),error);
        require(document.loadSong(staged,&error),error);
        require(document.readOnlyReason().isEmpty(),document.readOnlyReason());
        for (const auto &object : document.objects()) require(!object.isProtected(),object.protectedReason);
    }
    report->insert("sourceChartVersion",sourceVersion); report->insert("outputChartVersion",QStringLiteral("2.2.0"));
    decode(audio,document.audioPath()); auto request=requestFor(document,audio,difficulty,0); MusicAnalysis analysis;
    require(MusicFeatureAnalyzer::analyze(request,&analysis,&error),error); QStringList errors;
    report->insert("metrics",inspect(document.objects(),request,analysis,&errors));
    report->insert("formatValid",true); report->insert("playabilityPass",errors.isEmpty()); report->insert("errors",strings(errors));
    report->insert("removedBombs",bombs); report->insert("removedWalls",walls);
    report->insert("warnings",strings(analysis.warnings));
    inspectCoverage(document.objects(),request,analysis,report);
    require(treeHashes(source)==before,QStringLiteral("模型源目录在处理过程中改变，已停止导出。"));
    require(SongExporter::exportSong(document,output,0,audio.toolsDirectory(),&error),error);
    require(digest(QDir(output).filePath("song.ogg"))==digest(audioPath),QStringLiteral("导出音频与模型源音频不同。"));
    report->insert("outputAudioSha256",digest(QDir(output).filePath("song.ogg")));
    report->insert("sourceHashes",before);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc,argv); QCoreApplication::setApplicationName(QStringLiteral("lmsc-generation-trial"));
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("本地算法与现成模型的独立制谱试用；通过桌面约束检查仍须头显试玩。"));
    parser.addHelpOption(); parser.addPositionalArgument("command","describe | local | check");
    for (const auto &name : {QStringLiteral("project"),QStringLiteral("song"),QStringLiteral("difficulty"),QStringLiteral("seed"),
                            QStringLiteral("output"),QStringLiteral("report"),QStringLiteral("tools")}) parser.addOption(QCommandLineOption(name,name,"value"));
    if (!parser.parse(app.arguments())) { std::fprintf(stderr,"%s\n",parser.errorText().toUtf8().constData()); return 2; }
    if (parser.isSet("help")) parser.showHelp();
    const auto commands=parser.positionalArguments(); const auto reportPath=parser.value("report");
    QJsonObject report{{"schemaVersion",1},{"formatValid",false},{"playabilityPass",false},{"coveragePass",false},{"playableReady",false},{"valid",false}};
    QElapsedTimer elapsed; elapsed.start(); int exitCode=0; bool reportWritable=false;
    try {
        require(commands.size()==1 && QStringList{"describe","local","check"}.contains(commands.first()),QStringLiteral("需要 describe、local 或 check 命令。"));
        const auto command=commands.first(); report.insert("command",command);
        const QFileInfo sourcePath(command=="check" ? parser.value("song") : parser.value("project"));
        const auto sourceRoot=command=="check" || sourcePath.isDir() ? sourcePath.absoluteFilePath() : sourcePath.absolutePath();
        freshPath(reportPath,sourceRoot); reportWritable=true;
        if (command=="describe" || command=="local") {
            BeatmapDocument source; QString error; require(source.loadProject(parser.value("project"),&error),error);
            const auto description=sourceInfo(source); for (auto it=description.begin();it!=description.end();++it) report.insert(it.key(),it.value());
            if (command=="describe") { report.insert("formatValid",true); }
            else {
                const auto difficulty=parser.value("difficulty"); require(difficulty=="Hard" || difficulty=="Expert",QStringLiteral("试用难度只能为 Hard 或 Expert。"));
                require(source.timeMap().changes().isEmpty() && source.timeMap().firstBeatSeconds()>=0,QStringLiteral("本轮本地对照仅支持非负首拍、恒定 BPM 工程。"));
                const auto projectFolder=QFileInfo(source.projectPath()).absolutePath(); const auto before=treeHashes(projectFolder);
                const auto output=parser.value("output"); freshPath(output,projectFolder); bool seedOk=true;
                const auto seed=parser.isSet("seed")?parser.value("seed").toUInt(&seedOk):20261005u; require(seedOk,QStringLiteral("种子须为 uint32。"));
                AudioService audio; audio.setToolsDirectory(parser.value("tools")); require(audio.toolsAvailable(),QStringLiteral("找不到 FFmpeg 音频工具。"));
                decode(audio,source.audioPath()); auto request=requestFor(source,audio,difficulty,seed); MusicAnalysis analysis; GenerationDraft draft;
                require(MusicFeatureAnalyzer::analyze(request,&analysis,&error),error); require(LocalChartGenerator::generate(request,analysis,&draft,&error),error);
                QStringList errors; auto metrics=inspect(draft.objects,request,analysis,&errors); QJsonArray families;
                for(int count:draft.metrics.actionFamilyCounts) families.append(count); metrics.insert("actionFamilyCounts",families);
                report.insert("metrics",metrics); report.insert("formatValid",true); report.insert("playabilityPass",errors.isEmpty()); report.insert("errors",strings(errors));
                report.insert("warnings",strings(draft.warnings)); report.insert("difficulty",difficulty); report.insert("seed",double(seed));
                inspectCoverage(draft.objects,request,analysis,&report);
                BeatmapDocument generated;
                require(generated.createNew(source.audioPath(),source.title(),source.timeMap().baseBpm(),source.timeMap().firstBeatSeconds(),source.coverPath(),&error),error);
                require(generated.applyGeneratedChart(draft.objects,difficulty,request.profile.rank,generated.revision(),&error),error);
                require(treeHashes(projectFolder)==before,QStringLiteral("源工程在生成过程中改变，已停止导出。"));
                require(SongExporter::exportSong(generated,output,0,audio.toolsDirectory(),&error),error);
                require(digest(QDir(output).filePath("song.ogg"))==digest(source.audioPath()),QStringLiteral("本地对照音频与原工程不同。"));
                report.insert("outputAudioSha256",digest(QDir(output).filePath("song.ogg"))); report.insert("outputSongDirectory",QFileInfo(output).absoluteFilePath());
            }
        } else {
            const auto difficulty=parser.value("difficulty"); require(difficulty=="Hard" || difficulty=="Expert",QStringLiteral("试用难度只能为 Hard 或 Expert。"));
            AudioService audio; audio.setToolsDirectory(parser.value("tools")); require(audio.toolsAvailable(),QStringLiteral("找不到 FFmpeg 音频工具。"));
            checkSong(parser.value("song"),difficulty,parser.value("output"),audio,&report);
            report.insert("difficulty",difficulty); report.insert("outputSongDirectory",QFileInfo(parser.value("output")).absoluteFilePath());
        }
        const bool ready=report.value("formatValid").toBool() && report.value("playabilityPass").toBool() && report.value("coveragePass").toBool()
            && report.value("metrics").toObject().value("notes").toInt()>0;
        report.insert("playableReady",ready); report.insert("valid",ready);
    } catch (const std::exception &error) {
        exitCode=2; report.insert("operationError",QString::fromUtf8(error.what()));
        report.insert("playableReady",false); report.insert("valid",false);
        std::fprintf(stderr,"%s\n",error.what());
    }
    report.insert("elapsedSeconds",elapsed.elapsed()/1000.0); report.insert("exitCode",exitCode);
    if (reportWritable && !reportPath.isEmpty() && !QFileInfo::exists(reportPath)) {
        QString error;
        if (!QDir().mkpath(QFileInfo(reportPath).absolutePath()) || !ProjectStore::writeJson(reportPath,report,&error)) {
            std::fprintf(stderr,"report: %s\n",error.toUtf8().constData()); exitCode=2;
        }
    }
    const auto json=QJsonDocument(report).toJson(QJsonDocument::Compact); std::fwrite(json.constData(),1,size_t(json.size()),stdout); std::fputc('\n',stdout);
    return exitCode;
}
