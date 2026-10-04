#include "core/MtpDeleteService.h"

#include <QCoreApplication>
#include <QDir>
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
void check(bool passed, const QString &message) {
    QTextStream stream(passed ? stdout : stderr); stream.setCodec("UTF-8");
    stream << (passed ? QStringLiteral("通过：") : QStringLiteral("失败：")) << message << '\n';
    if (!passed) ++failures;
}
bool write(const QString &path, const QByteArray &data) {
    QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}
QJsonObject locator() {
    return {{"device", QStringLiteral("Pico Neo 3")}, {"segments", QJsonArray{QStringLiteral("内部共享存储空间"),
        "SoulTopia", "BeatNote", "Custom", QStringLiteral("合成|分类"), QStringLiteral("合成 $(literal) '|歌曲")}}};
}
MtpSongEntry song() {
    MtpSongEntry entry; const auto path = locator(); entry.deviceName = path.value("device").toString();
    entry.name = path.value("segments").toArray().last().toString(); entry.gameId = "oasis";
    entry.storageName = path.value("segments").toArray().first().toString(); entry.locator = QString::fromUtf8(QJsonDocument(path).toJson(QJsonDocument::Compact));
    return entry;
}
QString script(const QTemporaryDir &directory, const QString &body) {
    const QString path = directory.filePath(QStringLiteral("删除 $(literal) ' 测试.ps1"));
    write(path, QByteArray::fromHex("efbbbf") + (QStringLiteral(
        "param([string]$JobPath)\n$ErrorActionPreference='Stop'\n[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)\n"
        "$job=[IO.File]::ReadAllText($JobPath)|ConvertFrom-Json\n"
        "function Send($record){[Console]::WriteLine(($record|ConvertTo-Json -Depth 16 -Compress))}\ntry{\n")
        + body + QStringLiteral("\n}catch{Send @{type='error';message=$_.Exception.Message};exit 1}\n")).toUtf8());
    return path;
}
const QString prepareRecord = QStringLiteral(
    "[IO.File]::WriteAllText($job.planFile,'{}')\n"
    "Send @{type='prepared';locator=$job.locator;files=3;bytes=130;planToken=('a'*64)}\n");
const QString preparedAndDelete = QStringLiteral(
    "if($job.mode -ceq 'delete-prepare'){\n") + prepareRecord + QStringLiteral(
    "}elseif($job.mode -ceq 'delete'){\n"
    "if($job.planToken -cne ('a'*64) -or -not [IO.File]::Exists($job.planFile)){throw '原预备清单未传递'}\n"
    "Send @{type='deleting';locator=$job.locator}\nSend @{type='deleted';locator=$job.locator;verified=$true}\n"
    "}else{throw '模式不符'}\n");
struct Result { bool prepared = false, deleted = false, cancelled = false; QString error; MtpSongEntry entry; int files = 0; qint64 bytes = 0; };
Result waitFor(MtpDeleteService &service, const std::function<void()> &start) {
    Result result; QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    const auto prepared = QObject::connect(&service, &MtpDeleteService::deletionPrepared, &loop, [&](const MtpSongEntry &entry, int files, qint64 bytes) {
        result.prepared = true; result.entry = entry; result.files = files; result.bytes = bytes; loop.quit();
    });
    const auto deleted = QObject::connect(&service, &MtpDeleteService::songDeleted, &loop, [&] { result.deleted = true; loop.quit(); });
    const auto error = QObject::connect(&service, &MtpDeleteService::errorOccurred, &loop, [&](const QString &message) { result.error = message; if (!service.isBusy()) loop.quit(); });
    const auto cancelled = QObject::connect(&service, &MtpDeleteService::cancelled, &loop, [&] { result.cancelled = true; loop.quit(); });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] { result.error = QStringLiteral("测试超时"); service.cancel(); loop.quit(); });
    QTimer::singleShot(0, &loop, start); timer.start(15000); loop.exec();
    QObject::disconnect(prepared); QObject::disconnect(deleted); QObject::disconnect(error); QObject::disconnect(cancelled); return result;
}
const QString folderType = "27e2e392-a111-48e0-ab0c-e17705a05f85", fileType = "0085e0a6-8d34-45d7-bc5c-447e59c73d48";
const QString unspecifiedType = "28d8d31e-249c-454e-aabc-34883168e634", unspecifiedFormat = "30000000-ae6c-4804-98ba-c57b46965fe7";
QJsonObject object(const QString &id, const QString &parent, const QString &name, const QString &type, qint64 size = 0, bool storage = false) {
    return {{"Id", id}, {"Persistent", id + "-persistent"}, {"Parent", parent}, {"Name", name}, {"Type", type},
        {"Category", storage ? QStringLiteral("23f05bbc-15de-4c2a-a55b-a9af5ce412ef") : QStringLiteral("00000000-0000-0000-0000-000000000000")},
        {"OriginalFileName", name}, {"Format", type == folderType ? QStringLiteral("30010000-ae6c-4804-98ba-c57b46965fe7") : unspecifiedFormat}, {"Size", double(size)}};
}
QJsonObject syntheticJob(const QString &cancelFile) {
    const auto segments = locator().value("segments").toArray(); QJsonArray objects;
    QString parent = "DEVICE";
    for (int index = 0; index < segments.size(); ++index) {
        const QString id = "path-" + QString::number(index);
        objects.append(object(id, parent, segments[index].toString(), folderType, 0, index == 0)); parent = id;
    }
    objects.append(object("info", parent, "Info.dat", fileType, 100));
    objects.append(object("audio", parent, "song.ogg", "4ad2c85e-5e2d-45e5-8864-4f229e3c6cf0", 20));
    objects.append(object("resources", parent, QStringLiteral("资源|子目录"), folderType));
    objects.append(object("chart", "resources", "Easy.dat", fileType, 10));
    // 同分类的其他歌曲和分类外文件必须保持，不能用递归删除夹带。
    objects.append(object("other-song", "path-4", QStringLiteral("另一首歌曲"), folderType));
    objects.append(object("other-info", "other-song", "Info.dat", fileType, 100));
    objects.append(object("scores", "path-3", "scores.dat", fileType, 50));
    return {{"protocol", 1}, {"mode", "validate-delete"}, {"cancelFile", cancelFile}, {"planFile", QFileInfo(cancelFile).absolutePath() + "/plan.json"}, {"locator", locator()},
        {"gameId", "oasis"}, {"name", song().name}, {"objects", objects},
        {"resourceData", QJsonObject{{"info", QString::fromLatin1(QByteArray(100, 'i').toBase64())}, {"chart", QString::fromLatin1(QByteArray(10, 'c').toBase64())}}},
        {"infoText", QStringLiteral("{\"_songFilename\":\"song.ogg\",\"_difficultyBeatmapSets\":[]}")}};
}
QJsonObject runHelper(const QString &helper, const QString &jobPath, QJsonObject job, int *exit) {
    QFile::remove(job.value("cancelFile").toString()); QFile::remove(job.value("planFile").toString()); write(jobPath, QJsonDocument(job).toJson());
    QProcess process; process.start(helper, {"--job", jobPath});
    if (!process.waitForStarted(10000) || !process.waitForFinished(15000)) { process.kill(); process.waitForFinished(3000); *exit = -1; return {}; }
    *exit = process.exitCode(); QJsonObject record;
    for (const auto &line : process.readAllStandardOutput().split('\n')) {
        const auto document = QJsonDocument::fromJson(line.trimmed()); if (document.isObject()) record = document.object();
    }
    return record;
}
QJsonObject changeObject(QJsonObject job, const QString &id, const QString &key, const QJsonValue &value) {
    auto objects = job.value("objects").toArray();
    for (int index = 0; index < objects.size(); ++index) {
        auto item = objects[index].toObject(); if (item.value("Id").toString() == id) { item.insert(key, value); objects[index] = item; }
    }
    job.insert("objects", objects); return job;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv); QTemporaryDir temporary; MtpDeleteService service;
    check(temporary.isValid(), QStringLiteral("创建独立合成删除测试目录"));
    auto result = waitFor(service, [&] { service.deletePrepared(); });
    check(!result.deleted && !result.error.isEmpty(), QStringLiteral("不能绕过预备核对直接删除"));
    service.setHelperPath(temporary.filePath("missing.exe")); result = waitFor(service, [&] { service.prepareSong(song()); });
    check(!result.prepared && !result.error.isEmpty() && !service.isBusy(), QStringLiteral("工具缺失时不开始删除"));
    service.setHelperPath(script(temporary, preparedAndDelete));
    auto invalid = song(); invalid.isSong = false; result = waitFor(service, [&] { service.prepareSong(invalid); });
    check(!result.prepared && !result.error.isEmpty(), QStringLiteral("拒绝设备、游戏与分类节点"));
    invalid = song(); invalid.name += "wrong"; result = waitFor(service, [&] { service.prepareSong(invalid); });
    check(!result.prepared && !result.error.isEmpty(), QStringLiteral("界面歌曲名称必须与原定位匹配"));
    auto root = locator(); auto rootSegments = root.value("segments").toArray(); rootSegments.removeLast(); rootSegments.removeLast(); root.insert("segments", rootSegments);
    invalid = song(); invalid.name = "Custom"; invalid.locator = QString::fromUtf8(QJsonDocument(root).toJson());
    result = waitFor(service, [&] { service.prepareSong(invalid); }); check(!result.prepared && !result.error.isEmpty(), QStringLiteral("拒绝游戏歌曲根目录"));
    auto traversal = locator(); auto parts = traversal.value("segments").toArray(); parts[4] = ".."; traversal.insert("segments", parts);
    invalid = song(); invalid.locator = QString::fromUtf8(QJsonDocument(traversal).toJson());
    result = waitFor(service, [&] { service.prepareSong(invalid); }); check(!result.prepared && !result.error.isEmpty(), QStringLiteral("拒绝跨目录定位"));

    result = waitFor(service, [&] { service.prepareSong(song()); });
    check(result.prepared && result.files == 3 && result.bytes == 130 && !result.deleted, QStringLiteral("预备只读，保留中文、竖线及 shell 字面字符"));
    invalid = song(); invalid.location = QStringLiteral("伪造其他分类"); invalid.gameName = QStringLiteral("伪造其他游戏");
    result = waitFor(service, [&] { service.prepareSong(invalid); });
    check(result.prepared && result.entry.gameName == QStringLiteral("星穹绿洲")
        && result.entry.location == QStringLiteral("内部共享存储空间\\SoulTopia\\BeatNote\\Custom\\合成|分类\\合成 $(literal) '|歌曲"),
        QStringLiteral("确认位置与游戏名称由已核对定位重新生成，不能伪造显示对象"));
    result = waitFor(service, [&] { service.deletePrepared(); });
    check(result.deleted && result.error.isEmpty(), QStringLiteral("确认后携带同一预备清单完成单首删除"));
    result = waitFor(service, [&] { service.deletePrepared(); }); check(!result.deleted && !result.error.isEmpty(), QStringLiteral("完成后预备结果不可复用"));
    result = waitFor(service, [&] { service.prepareSong(song()); }); service.cancel();
    result = waitFor(service, [&] { service.deletePrepared(); }); check(!result.deleted && !result.error.isEmpty(), QStringLiteral("取消确认后预备结果失效"));
    for (const auto &field : {QStringLiteral("files=1.5;bytes=130"), QStringLiteral("files=3;bytes=1.5"), QStringLiteral("files=3;bytes=-1"), QStringLiteral("files=10001;bytes=130")}) {
        service.setHelperPath(script(temporary, QStringLiteral("[IO.File]::WriteAllText($job.planFile,'{}')\nSend @{type='prepared';locator=$job.locator;") + field + ";planToken=('a'*64)}\n"));
        result = waitFor(service, [&] { service.prepareSong(song()); });
        check(!result.prepared && !result.error.isEmpty(), QStringLiteral("异常预备计数不能截断接受：%1").arg(field));
    }
    service.setHelperPath(script(temporary, QStringLiteral("Send @{type='progress';message='只读核对';percent=0}\nwhile(-not [IO.File]::Exists($job.cancelFile)){Start-Sleep -Milliseconds 20}\nSend @{type='cancelled';deletionStarted=$false}\nexit 2\n")));
    auto progress = QObject::connect(&service, &MtpDeleteService::taskProgress, &service, [&](const QString &, int percent) { if (percent == 0) service.cancel(); });
    result = waitFor(service, [&] { service.prepareSong(song()); }); QObject::disconnect(progress);
    check(result.cancelled && result.error.isEmpty() && !result.deleted, QStringLiteral("预备阶段取消明确未删除"));
    service.setHelperPath(script(temporary, QStringLiteral("if($job.mode -ceq 'delete-prepare'){\n") + prepareRecord + QStringLiteral(
        "}else{Send @{type='deleting';locator=$job.locator}\nSend @{type='progress';message='已开始';percent=0}\n"
        "while(-not [IO.File]::Exists($job.cancelFile)){Start-Sleep -Milliseconds 20}\nSend @{type='cancelled';deletionStarted=$true}\nexit 2}\n")));
    result = waitFor(service, [&] { service.prepareSong(song()); });
    progress = QObject::connect(&service, &MtpDeleteService::taskProgress, &service, [&](const QString &, int percent) { if (percent == 0) service.cancel(); });
    result = waitFor(service, [&] { service.deletePrepared(); }); QObject::disconnect(progress);
    check(!result.cancelled && !result.deleted && result.error.contains(QStringLiteral("部分删除")), QStringLiteral("删除中停止只报告部分删除风险，不再发送安全取消信号"));
    service.setHelperPath(script(temporary, QStringLiteral("if($job.mode -ceq 'delete-prepare'){\n") + prepareRecord + QStringLiteral(
        "}else{Send @{type='deleted';locator=$job.locator;verified=$true}}\n")));
    result = waitFor(service, [&] { service.prepareSong(song()); }); result = waitFor(service, [&] { service.deletePrepared(); });
    check(!result.deleted && !result.error.isEmpty(), QStringLiteral("缺少删除开始和核对记录不报告成功"));
    service.setHelperPath(script(temporary, QStringLiteral("if($job.mode -ceq 'delete-prepare'){\n") + prepareRecord + QStringLiteral(
        "}else{Send @{type='deleting';locator=$job.locator}\nSend @{type='deleted';locator=@{device='other';segments=@()};verified=$true}}\n")));
    result = waitFor(service, [&] { service.prepareSong(song()); }); result = waitFor(service, [&] { service.deletePrepared(); });
    check(!result.deleted && result.error.contains(QStringLiteral("核对")), QStringLiteral("拒绝其他目标的删除结果"));

    const int helperIndex = application.arguments().indexOf("--helper") + 1;
    const QString helper = helperIndex > 0 && helperIndex < application.arguments().size() ? application.arguments()[helperIndex] : service.helperPath();
    if (!QFileInfo(helper).isFile() || QFileInfo(helper).suffix().compare("exe", Qt::CaseInsensitive) != 0) {
        check(false, QStringLiteral("缺少真实 WPD helper，无法验证生产枚举与删除边界")); return 1;
    }
    const QString jobPath = temporary.filePath("validation-job.json"), cancelFile = temporary.filePath("validation-cancel");
    auto job = syntheticJob(cancelFile); int exitCode; auto record = runHelper(helper, jobPath, job, &exitCode);
    const auto deleted = record.value("deletedIds").toArray(), remaining = record.value("remainingIds").toArray();
    check(exitCode == 0 && record.value("type") == "delete-validated" && record.value("deviceAccessed") == false
        && deleted.size() == 5 && deleted[deleted.size() - 2] == "info" && deleted.last() == "path-5"
        && remaining.contains("other-song") && remaining.contains("other-info") && remaining.contains("scores"),
        QStringLiteral("真实 helper 使用生产逻辑模拟非递归删除，Info 最后，其他歌曲与成绩不变：%1").arg(record.value("message").toString()));
    auto unspecified = changeObject(changeObject(job, "info", "Type", unspecifiedType), "chart", "Type", unspecifiedType);
    record = runHelper(helper, jobPath, unspecified, &exitCode);
    check(exitCode == 0 && record.value("type") == "delete-validated" && record.value("deviceAccessed") == false,
        QStringLiteral("未指定类型 .dat 在原文件名、文件格式、大小、默认流及无子项证据齐全时可删除：%1").arg(record.value("message").toString()));
    for (const auto &invalidEvidence : {changeObject(unspecified, "info", "OriginalFileName", QString()),
            changeObject(unspecified, "info", "OriginalFileName", QStringLiteral("other.dat")),
            changeObject(unspecified, "info", "Format", QStringLiteral("00000000-0000-0000-0000-000000000000")),
            changeObject(unspecified, "info", "Size", QStringLiteral("18446744073709551615")),
            changeObject(unspecified, "info", "Category", QStringLiteral("23f05bbc-15de-4c2a-a55b-a9af5ce412ef"))}) {
        record = runHelper(helper, jobPath, invalidEvidence, &exitCode);
        check(exitCode == 1 && record.value("deletionStarted") == false,
            QStringLiteral("未指定类型缺少原名、文件格式、已知大小或普通内容类别时拒绝"));
    }
    auto missingStream = unspecified; auto resourceData = missingStream.value("resourceData").toObject(); resourceData.remove("info");
    missingStream.insert("resourceData", resourceData); record = runHelper(helper, jobPath, missingStream, &exitCode);
    check(exitCode == 1 && record.value("deletionStarted") == false, QStringLiteral("无默认可读数据流的未指定虚拟对象不能删除"));
    auto shortStream = unspecified; resourceData = shortStream.value("resourceData").toObject();
    resourceData.insert("info", QString::fromLatin1(QByteArray(99, 'i').toBase64())); shortStream.insert("resourceData", resourceData);
    record = runHelper(helper, jobPath, shortStream, &exitCode);
    check(exitCode == 1 && record.value("deletionStarted") == false, QStringLiteral("默认文件流未读全不能作为实体文件证明"));
    auto withChild = unspecified; auto proofObjects = withChild.value("objects").toArray();
    proofObjects.append(object("virtual-child", "info", "child.dat", fileType, 1)); withChild.insert("objects", proofObjects);
    record = runHelper(helper, jobPath, withChild, &exitCode);
    check(exitCode == 1 && record.value("deletionStarted") == false, QStringLiteral("带子对象的未指定类型不能作为叶文件删除"));
    auto changedProof = unspecified; changedProof.insert("mutation", "file-evidence"); record = runHelper(helper, jobPath, changedProof, &exitCode);
    check(exitCode == 1 && record.value("deletionStarted") == false, QStringLiteral("确认后实体文件证明 SHA256 变化时拒绝删除"));
    for (const auto &mutation : {"persistent", "size", "info", "new-file", "plan-token", "plan-file", "during-delete", "during-disconnect"}) {
        auto changed = job; changed.insert("mutation", mutation); record = runHelper(helper, jobPath, changed, &exitCode);
        check(exitCode == 1 && record.value("type") == "error", QStringLiteral("生产逻辑拒绝确认后的变化或断连：%1").arg(mutation));
    }
    auto changed = job; changed.insert("mutation", "during-cancel"); record = runHelper(helper, jobPath, changed, &exitCode);
    check(exitCode == 2 && record.value("deletionStarted") == true, QStringLiteral("生产逐步删除取消如实报告可能已部分删除"));
    changed = job; changed.insert("deleteFailure", "first"); record = runHelper(helper, jobPath, changed, &exitCode);
    check(exitCode == 1 && record.value("type") == "error", QStringLiteral("驱动首次删除失败后停止，不继续处理资源"));
    auto objects = job.value("objects").toArray(); objects.append(object("nested-info", "resources", "Info.dat", fileType, 1));
    changed = job; changed.insert("objects", objects); record = runHelper(helper, jobPath, changed, &exitCode);
    check(exitCode == 1 && record.value("deletionStarted") == false, QStringLiteral("生产枚举拒绝嵌套歌曲，尚未开始删除"));
    objects = job.value("objects").toArray(); objects.append(object("duplicate-info", "path-5", "info.DAT", fileType, 1));
    changed = job; changed.insert("objects", objects); record = runHelper(helper, jobPath, changed, &exitCode);
    check(exitCode == 1 && record.value("deletionStarted") == false, QStringLiteral("生产枚举拒绝不区分大小写的重复资源"));
    for (const auto &invalidJob : {changeObject(job, "chart", "Type", QStringLiteral("00000000-0000-0000-0000-000000000001")),
            changeObject(job, "chart", "Persistent", QString()), changeObject(job, "chart", "Parent", QStringLiteral("other-song")),
            changeObject(job, "resources", "Name", QStringLiteral("../escape"))}) {
        record = runHelper(helper, jobPath, invalidJob, &exitCode);
        // 移出歌曲的文件不能被夹带删除；其余非法对象必须拒绝。
        const auto changedChart = invalidJob.value("objects").toArray()[9].toObject();
        if (changedChart.value("Parent") == "other-song")
            check(exitCode == 0 && record.value("remainingIds").toArray().contains("chart"), QStringLiteral("歌曲外对象不进入删除清单"));
        else check(exitCode == 1, QStringLiteral("生产枚举拒绝未知类型、无稳定标识及非法路径"));
    }
    changed = job; changed.insert("locator", root); changed.insert("name", "Custom"); record = runHelper(helper, jobPath, changed, &exitCode);
    check(exitCode == 1 && record.value("deletionStarted") == false, QStringLiteral("helper 独立拒绝删除游戏根目录"));
    changed = job; auto categoryLocator = locator(); auto categoryParts = categoryLocator.value("segments").toArray(); categoryParts.removeLast();
    categoryLocator.insert("segments", categoryParts); changed.insert("locator", categoryLocator); changed.insert("name", categoryParts.last());
    record = runHelper(helper, jobPath, changed, &exitCode);
    check(exitCode == 1 && record.value("deletionStarted") == false, QStringLiteral("helper 独立拒绝没有 Info 的分类目录"));
    QFile::remove(cancelFile);
    return failures ? 1 : 0;
}
