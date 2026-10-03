#include "core/MtpExportService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <functional>

namespace {
int failures = 0;
void check(bool condition, const QString &message) {
    QTextStream output(condition ? stdout : stderr); output.setCodec("UTF-8");
    output << (condition ? QStringLiteral("通过：") : QStringLiteral("失败：")) << message << '\n';
    if (!condition) ++failures;
}
bool write(const QString &path, const QByteArray &data) {
    QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}
QString makeScript(const QTemporaryDir &directory, const QString &body) {
    const QString path = directory.path() + QStringLiteral("/设备 导出 $(literal).ps1");
    write(path, QByteArray::fromHex("efbbbf") + (QStringLiteral(
        "param([string]$JobPath)\n$ErrorActionPreference='Stop'\n[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)\n"
        "$job=[IO.File]::ReadAllText($JobPath)|ConvertFrom-Json\n"
        "function Send($value){[Console]::WriteLine(($value|ConvertTo-Json -Depth 12 -Compress))}\ntry {\n") + body
        + QStringLiteral("\n} catch {Send @{type='error';message=$_.Exception.Message};exit 1}\n")).toUtf8());
    return path;
}
QString song(const QTemporaryDir &directory, const QString &name = QStringLiteral("中文歌曲 $(abc) '-by光剑曲谱")) {
    const QString folder = directory.path() + QStringLiteral("/光剑曲谱制作/") + name; QDir().mkpath(folder + QStringLiteral("/资源"));
    write(folder + QStringLiteral("/资源/song.ogg"), QByteArray("synthetic-audio"));
    write(folder + QStringLiteral("/Easy.dat"), QByteArray("{\"_version\":\"2.2.0\",\"_notes\":[],\"_events\":[],\"_obstacles\":[]}\n"));
    const QJsonObject chart{{QStringLiteral("_difficulty"), QStringLiteral("Easy")}, {QStringLiteral("_beatmapFilename"), QStringLiteral("Easy.dat")}};
    const QJsonObject set{{QStringLiteral("_beatmapCharacteristicName"), QStringLiteral("Standard")}, {QStringLiteral("_difficultyBeatmaps"), QJsonArray{chart}}};
    const QJsonObject info{{QStringLiteral("_version"), QStringLiteral("2.0.0")}, {QStringLiteral("_songFilename"), QStringLiteral("资源/song.ogg")},
        {QStringLiteral("_coverImageFilename"), QString()}, {QStringLiteral("_difficultyBeatmapSets"), QJsonArray{set}}};
    write(folder + QStringLiteral("/Info.dat"), QJsonDocument(info).toJson()); return folder;
}
struct Outcome { bool completed = false, cancelled = false; QString error, location; QVector<MtpExportDestination> destinations; };
Outcome waitFor(MtpExportService &service, const std::function<void()> &start, int timeoutMs = 15000) {
    Outcome result; QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
    auto listed = QObject::connect(&service, &MtpExportService::destinationsListed, &loop, [&](const QVector<MtpExportDestination> &items) { result.completed = true; result.destinations = items; loop.quit(); });
    auto uploaded = QObject::connect(&service, &MtpExportService::songUploaded, &loop, [&](const QString &, const QString &path) { result.completed = true; result.location = path; loop.quit(); });
    auto error = QObject::connect(&service, &MtpExportService::errorOccurred, &loop, [&](const QString &message) { result.error = message; if (!service.isBusy()) loop.quit(); });
    auto cancelled = QObject::connect(&service, &MtpExportService::cancelled, &loop, [&] { result.cancelled = true; loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { result.error = QStringLiteral("测试超时"); service.cancel(); loop.quit(); });
    QTimer::singleShot(0, &loop, start); timeout.start(timeoutMs); loop.exec();
    QObject::disconnect(listed); QObject::disconnect(uploaded); QObject::disconnect(error); QObject::disconnect(cancelled); return result;
}
MtpExportDestination destination() {
    MtpExportDestination result; result.deviceId = QStringLiteral("PICO Stable Device $(abc) '"); result.storageId = QStringLiteral("storage-persistent");
    result.rootId = QStringLiteral("game-persistent"); result.gameId = QStringLiteral("lightband"); result.deviceName = QStringLiteral("Pico Neo 3");
    result.storageName = QStringLiteral("内部共享存储空间"); result.gameName = QStringLiteral("光之乐团"); return result;
}
QJsonObject validationJob(const QString &folder, const QString &cancelFile) {
    QJsonArray files;
    QDirIterator iterator(folder, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = QDir(folder).relativeFilePath(iterator.next());
        QFile file(folder + QLatin1Char('/') + path); file.open(QIODevice::ReadOnly); const auto data = file.readAll();
        files.append(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("size"), double(data.size())},
            {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())}});
    }
    const QString audio = QFileInfo::exists(folder + QStringLiteral("/声音.ogg")) ? QStringLiteral("声音.ogg") : QStringLiteral("资源/song.ogg");
    return QJsonObject{{QStringLiteral("protocol"), 1}, {QStringLiteral("mode"), QStringLiteral("validate")}, {QStringLiteral("cancelFile"), cancelFile},
        {QStringLiteral("localFolder"), folder}, {QStringLiteral("files"), files}, {QStringLiteral("references"), QJsonArray{audio, QStringLiteral("Easy.dat")}}};
}
QJsonObject validateWithHelper(const QString &helper, const QString &taskPath, const QJsonObject &job, int *exitCode) {
    write(taskPath, QJsonDocument(job).toJson()); QProcess process;
    process.start(helper, {QStringLiteral("--job"), taskPath});
    if (!process.waitForStarted(10000) || !process.waitForFinished(20000)) { process.kill(); process.waitForFinished(3000); *exitCode = -1; return {}; }
    *exitCode = process.exitCode(); return QJsonDocument::fromJson(process.readAllStandardOutput().trimmed()).object();
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--device-list"))) {
        MtpExportService service; const int index = application.arguments().indexOf(QStringLiteral("--device-list")) + 1;
        if (index < application.arguments().size()) service.setHelperPath(application.arguments()[index]);
        const auto result = waitFor(service, [&] { service.listDestinations(); }, 120000);
        check(result.completed && result.error.isEmpty(), QStringLiteral("Qt32 调用 64 位 WPD 辅助程序读取设备游戏目录"));
        for (const auto &item : result.destinations) check(!item.storageId.isEmpty() && !item.rootId.isEmpty(), item.deviceName + QStringLiteral(" / ") + item.gameName);
        if (!result.error.isEmpty()) check(false, result.error); return failures ? 1 : 0;
    }
    QTemporaryDir scripts(QDir::tempPath() + QStringLiteral("/lmsc-wpd-tests-XXXXXX")); QTemporaryDir data;
    check(scripts.isValid() && data.isValid(), QStringLiteral("创建隔离测试目录"));
    const QString folder = song(data), originalHash = QString::fromLatin1(QCryptographicHash::hash(QByteArray("synthetic-audio"), QCryptographicHash::Sha256).toHex());
    MtpExportService service; const auto target = destination();
    service.setHelperPath(scripts.path() + QStringLiteral("/missing.exe"));
    auto result = waitFor(service, [&] { service.listDestinations(); });
    check(!result.completed && !result.error.isEmpty() && !service.isBusy(), QStringLiteral("工具缺失时失败，保留非忙状态"));

    service.setHelperPath(makeScript(scripts, QStringLiteral(
        "if($job.mode -cne 'list' -or $job.protocol -ne 1){throw '任务模式无效'}\n"
        "Send @{type='destinations';destinations=@(@{deviceId='device';storageId='storage';rootId='root-1';gameId='oasis';deviceName='Pico Neo 3';storageName='内部共享存储空间';gameName='星穹绿洲'},@{deviceId='device';storageId='storage';rootId='root-2';gameId='lightband';gameName='光之乐团'})}\n")));
    result = waitFor(service, [&] { service.listDestinations(); });
    check(result.completed && result.destinations.size() == 2 && result.destinations[1].gameName == QStringLiteral("光之乐团"), QStringLiteral("列出两游戏目标，允许空歌曲根，保留持久标识"));
    service.setHelperPath(makeScript(scripts, QStringLiteral("Send @{type='destinations';destinations=@(@{deviceId='device';storageId='storage';rootId='root';gameId='other'})}\n")));
    result = waitFor(service, [&] { service.listDestinations(); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("拒绝白名单之外游戏"));

    const QString upload = QStringLiteral(
        "if($job.mode -cne 'upload' -or $job.destination.deviceId -cne 'PICO Stable Device $(abc) '''){throw '字面设备标识损坏'}\n"
        "if(@($job.files).Count -ne 3 -or @($job.references).Count -ne 2){throw '资源或引用清单错误'}\n"
        "foreach($item in $job.files){$path=Join-Path $job.localFolder $item.path; $sha=[Security.Cryptography.SHA256]::Create(); try{$hash=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($path))).Replace('-','').ToLowerInvariant(); if($hash -cne $item.sha256){throw '原清单散列错误'}}finally{$sha.Dispose()}}\n"
        "Send @{type='created';location='Pico Neo 3\\光之乐团\\光剑曲谱制作\\中文歌曲-by光剑曲谱'}\n"
        "Send @{type='uploaded';localFolder=$job.localFolder;location='Pico Neo 3\\光之乐团\\光剑曲谱制作\\中文歌曲-by光剑曲谱';verified=$true}\n");
    service.setHelperPath(makeScript(scripts, upload));
    result = waitFor(service, [&] { service.uploadSong(folder, target); });
    check(result.completed && result.error.isEmpty() && !result.location.isEmpty(), QStringLiteral("独立 JSON 任务传递中文和 shell 字面字符，完整清单与回读完成协议：%1").arg(result.error));
    QFile audio(folder + QStringLiteral("/资源/song.ogg")); audio.open(QIODevice::ReadOnly);
    check(QString::fromLatin1(QCryptographicHash::hash(audio.readAll(), QCryptographicHash::Sha256).toHex()) == originalHash, QStringLiteral("传输服务保留本地完整副本")); audio.close();

    service.setHelperPath(makeScript(scripts, QStringLiteral("Send @{type='uploaded';localFolder=$job.localFolder;location='位置';verified=$false}\n")));
    result = waitFor(service, [&] { service.uploadSong(folder, target); });
    check(!result.completed && result.error.contains(QStringLiteral("回读校验")), QStringLiteral("校验失败不能报告上传成功"));
    service.setHelperPath(makeScript(scripts, QStringLiteral("Send @{type='uploaded';localFolder='C:/unexpected';location='位置';verified=$true}\n")));
    result = waitFor(service, [&] { service.uploadSong(folder, target); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("拒绝本地目标不匹配的结果"));
    service.setHelperPath(makeScript(scripts, QStringLiteral("Send @{type='uploaded';localFolder=$job.localFolder;location='位置';verified=$true}\nexit 1\n")));
    result = waitFor(service, [&] { service.uploadSong(folder, target); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("即使有完成记录，非零退出也不算成功"));
    service.setHelperPath(makeScript(scripts, QStringLiteral("Send @{type='destinations';destinations=@()}\nSend @{type='destinations';destinations=@()}\n")));
    result = waitFor(service, [&] { service.listDestinations(); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("拒绝重复完成记录"));
    service.setHelperPath(makeScript(scripts, QStringLiteral("Send @{type='progress';message='读取中';percent=10}\n")));
    result = waitFor(service, [&] { service.listDestinations(); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("缺少终结记录时不算成功"));

    service.setHelperPath(makeScript(scripts, QStringLiteral(
        "Send @{type='created';location='设备\\光剑曲谱制作\\本次新目录'}\nSend @{type='progress';message='传输中';percent=0}\n"
        "while(-not [IO.File]::Exists($job.cancelFile)){Start-Sleep -Milliseconds 30}\nSend @{type='cancelled';location='设备\\光剑曲谱制作\\本次新目录'}\nexit 2\n")));
    auto onProgress = QObject::connect(&service, &MtpExportService::taskProgress, &service, [&](const QString &, int percent) { if (percent == 0) service.cancel(); });
    result = waitFor(service, [&] { service.uploadSong(folder, target); }); QObject::disconnect(onProgress);
    check(result.cancelled && !result.completed && result.error.contains(QStringLiteral("本次新目录")) && QFileInfo::exists(folder + QStringLiteral("/Info.dat")), QStringLiteral("协作取消等待工具退出，提示新残留目录且保留电脑副本"));
    onProgress = QObject::connect(&service, &MtpExportService::taskProgress, &service, [&](const QString &, int) { service.cancel(); });
    result = waitFor(service, [&] { service.listDestinations(); }); QObject::disconnect(onProgress);
    check(result.cancelled && !result.completed && !service.isBusy(), QStringLiteral("启动进度同步取消不重新启动子进程"));
    const QString preparing = song(data, QStringLiteral("准备取消-by光剑曲谱"));
    QFile large(preparing + QStringLiteral("/资源/song.ogg")); large.open(QIODevice::WriteOnly | QIODevice::Truncate);
    const QByteArray block(1024 * 1024, 's'); for (int index = 0; index < 32; ++index) large.write(block); large.close();
    bool busyBeforeHash = false; QElapsedTimer elapsed; elapsed.start();
    onProgress = QObject::connect(&service, &MtpExportService::taskProgress, &service, [&](const QString &message, int) {
        if (message == QStringLiteral("校验电脑歌曲并准备设备导出")) {
            busyBeforeHash = service.isBusy(); QTimer::singleShot(0, &service, &MtpExportService::cancel);
        }
    });
    result = waitFor(service, [&] { service.uploadSong(preparing, target); }); QObject::disconnect(onProgress);
    check(result.cancelled && !result.completed && result.error.isEmpty() && busyBeforeHash && elapsed.elapsed() < 5000,
          QStringLiteral("大资源后台散列准备已设busy，GUI事件能及时取消，工具不开始上传"));

    service.setHelperPath(makeScript(scripts, upload));
    MtpExportDestination invalid = target; invalid.rootId.clear(); result = waitFor(service, [&] { service.uploadSong(folder, invalid); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("拒绝缺失持久标识的设备定位"));
    result = waitFor(service, [&] { service.uploadSong(data.path(), target); });
    check(!result.completed && result.error.contains(QStringLiteral("assets-*")), QStringLiteral("不允许上传工程或任意外部目录"));
    const QString missing = song(data, QStringLiteral("缺失资源-by光剑曲谱")); QFile::remove(missing + QStringLiteral("/Easy.dat"));
    result = waitFor(service, [&] { service.uploadSong(missing, target); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("Info 引用缺失时在启动工具前拒绝"));
    const QString unsafe = song(data, QStringLiteral("非法引用-by光剑曲谱"));
    QFile unsafeInfo(unsafe + QStringLiteral("/Info.dat")); unsafeInfo.open(QIODevice::ReadOnly); auto object = QJsonDocument::fromJson(unsafeInfo.readAll()).object(); unsafeInfo.close();
    object.insert(QStringLiteral("_songFilename"), QStringLiteral("../outside.ogg")); write(unsafe + QStringLiteral("/Info.dat"), QJsonDocument(object).toJson());
    result = waitFor(service, [&] { service.uploadSong(unsafe, target); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("拒绝穿越文件引用"));
    const QString suffix = song(data, QStringLiteral("中文歌曲-by光剑曲谱-10"));
    service.setHelperPath(makeScript(scripts, QStringLiteral("Send @{type='uploaded';localFolder=$job.localFolder;location='同名第十首';verified=$true}\n")));
    result = waitFor(service, [&] { service.uploadSong(suffix, target); });
    check(result.completed && result.error.isEmpty(), QStringLiteral("允许十首以上的合法重名后缀"));
    const QString alias = song(data, QStringLiteral("声音别名-by光剑曲谱"));
    QFile aliasInfo(alias + QStringLiteral("/Info.dat")); aliasInfo.open(QIODevice::ReadOnly); auto aliasObject = QJsonDocument::fromJson(aliasInfo.readAll()).object(); aliasInfo.close();
    aliasObject.insert(QStringLiteral("_songFilename"), QStringLiteral("资源/song.egg")); const auto aliasBytes = QJsonDocument(aliasObject).toJson(); write(alias + QStringLiteral("/Info.dat"), aliasBytes);
    service.setHelperPath(makeScript(scripts, QStringLiteral("if(@($job.references) -notcontains '资源/song.ogg'){throw '音频别名未解析'}\nSend @{type='uploaded';localFolder=$job.localFolder;location='声音别名';verified=$true}\n")));
    result = waitFor(service, [&] { service.uploadSong(alias, target); });
    aliasInfo.open(QIODevice::ReadOnly);
    check(result.completed && result.error.isEmpty() && aliasInfo.readAll() == aliasBytes, QStringLiteral("PICO .egg 声明可匹配实际 .ogg，Info 原文不变")); aliasInfo.close();
    const QString uniqueAlias = song(data, QStringLiteral("唯一声音别名-by光剑曲谱"));
    QFile::rename(uniqueAlias + QStringLiteral("/资源/song.ogg"), uniqueAlias + QStringLiteral("/声音.ogg"));
    write(uniqueAlias + QStringLiteral("/Info.dat"), aliasBytes);
    service.setHelperPath(makeScript(scripts, QStringLiteral("if(@($job.references) -notcontains '声音.ogg'){throw '唯一音频别名未解析'}\nSend @{type='uploaded';localFolder=$job.localFolder;location='唯一声音别名';verified=$true}\n")));
    result = waitFor(service, [&] { service.uploadSong(uniqueAlias, target); });
    check(result.completed && result.error.isEmpty(), QStringLiteral("旧Egg名称与Ogg不同，Info同目录仅一个Ogg时沿用核心别名兼容"));
    write(uniqueAlias + QStringLiteral("/另一个.ogg"), QByteArray("another"));
    result = waitFor(service, [&] { service.uploadSong(uniqueAlias, target); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("多个Ogg候选时拒绝歧义，不擅自选择声音"));
    QFile::remove(uniqueAlias + QStringLiteral("/另一个.ogg"));

    // 真实 C# 校验入口只访问本机合成资源，不枚举、不写入设备。
    MtpExportService production; const QString realHelper = production.helperPath();
    check(QFileInfo(realHelper).isFile() && !realHelper.endsWith(QStringLiteral(".ps1")), QStringLiteral("存在随构建部署的真实 64 位 WPD 工具"));
    if (QFileInfo(realHelper).isFile()) {
        const QString taskPath = scripts.path() + QStringLiteral("/offline-validation.json"), cancelFile = scripts.path() + QStringLiteral("/cancel-offline");
        int exitCode = -1; auto job = validationJob(folder, cancelFile);
        auto record = validateWithHelper(realHelper, taskPath, job, &exitCode);
        check(exitCode == 0 && record.value(QStringLiteral("type")).toString() == QStringLiteral("validated") && record.value(QStringLiteral("files")).toInt() == 3,
              QStringLiteral("真实 C# 工具验证完整清单、SHA256 与嵌套引用，无设备写入"));
        auto wrongFiles = job.value(QStringLiteral("files")).toArray(); auto wrongHash = wrongFiles[1].toObject(); wrongHash.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0'))); wrongFiles[1] = wrongHash;
        auto badJob = job; badJob.insert(QStringLiteral("files"), wrongFiles); record = validateWithHelper(realHelper, taskPath, badJob, &exitCode);
        check(exitCode != 0 && record.value(QStringLiteral("message")).toString().contains(QStringLiteral("发生变化")), QStringLiteral("真实工具拒绝资源散列变化"));
        badJob = job; badJob.insert(QStringLiteral("references"), QJsonArray{QStringLiteral("../外部文件")}); record = validateWithHelper(realHelper, taskPath, badJob, &exitCode);
        check(exitCode != 0, QStringLiteral("真实工具拒绝篡改的引用清单"));
        badJob = job; badJob.insert(QStringLiteral("localFolder"), data.path()); record = validateWithHelper(realHelper, taskPath, badJob, &exitCode);
        check(exitCode != 0 && record.value(QStringLiteral("message")).toString().contains(QStringLiteral("assets-*")), QStringLiteral("真实工具独立限制完整导出根，不能仅靠 Qt 校验"));
        record = validateWithHelper(realHelper, taskPath, validationJob(alias, cancelFile), &exitCode);
        check(exitCode == 0 && record.value(QStringLiteral("type")).toString() == QStringLiteral("validated"), QStringLiteral("真实工具保留 PICO .egg/.ogg 别名兼容"));
        record = validateWithHelper(realHelper, taskPath, validationJob(uniqueAlias, cancelFile), &exitCode);
        check(exitCode == 0 && record.value(QStringLiteral("type")).toString() == QStringLiteral("validated"), QStringLiteral("真实工具支持Info同目录唯一Ogg别名"));
        write(uniqueAlias + QStringLiteral("/另一个.ogg"), QByteArray("another"));
        record = validateWithHelper(realHelper, taskPath, validationJob(uniqueAlias, cancelFile), &exitCode);
        check(exitCode != 0, QStringLiteral("真实工具拒绝多个Ogg歧义"));
        write(cancelFile, QByteArray("cancel")); record = validateWithHelper(realHelper, taskPath, job, &exitCode);
        check(exitCode == 2 && record.value(QStringLiteral("type")).toString() == QStringLiteral("cancelled"), QStringLiteral("真实工具识别取消标志，验证入口不开始任务"));
    }
    return failures ? 1 : 0;
}
