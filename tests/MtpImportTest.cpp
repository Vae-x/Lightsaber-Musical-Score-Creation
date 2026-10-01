#include "core/MtpImportService.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <functional>

namespace {
int failures = 0;
void check(bool condition, const QString &message) {
    QTextStream output(condition ? stdout : stderr);
    output.setCodec("UTF-8");
    output << (condition ? QStringLiteral("通过：") : QStringLiteral("失败：")) << message << '\n';
    if (!condition) ++failures;
}
QString makeScript(const QTemporaryDir &directory, const QString &body) {
    const QString path = directory.path() + QStringLiteral("/设备 导入测试.ps1");
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    script.write(QByteArray::fromHex("efbbbf"));
    script.write((QStringLiteral("$ErrorActionPreference='Stop'\n[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)\n") + body).toUtf8());
    return path;
}
struct Outcome {
    bool completed = false;
    bool cancelled = false;
    QString error;
    QString path;
    QVector<MtpSongEntry> songs;
};
Outcome waitFor(MtpImportService &service, const std::function<void()> &start, int timeoutMs = 10000) {
    Outcome result;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    auto list = QObject::connect(&service, &MtpImportService::songsListed, &loop, [&](const QVector<MtpSongEntry> &songs) { result.completed = true; result.songs = songs; loop.quit(); });
    auto imported = QObject::connect(&service, &MtpImportService::songImported, &loop, [&](const QString &path) { result.completed = true; result.path = path; loop.quit(); });
    auto error = QObject::connect(&service, &MtpImportService::errorOccurred, &loop, [&](const QString &message) { result.error = message; loop.quit(); });
    auto cancelled = QObject::connect(&service, &MtpImportService::cancelled, &loop, [&] { result.cancelled = true; loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { result.error = QStringLiteral("测试超时"); service.cancel(); loop.quit(); });
    QTimer::singleShot(0, &loop, start);
    timeout.start(timeoutMs);
    loop.exec();
    QObject::disconnect(list); QObject::disconnect(imported); QObject::disconnect(error); QObject::disconnect(cancelled);
    return result;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--device-check"))) {
        MtpImportService device;
        const int scriptIndex = application.arguments().indexOf(QStringLiteral("--device-check")) + 1;
        if (scriptIndex < application.arguments().size()) device.setScriptPath(application.arguments()[scriptIndex]);
        auto listed = waitFor(device, [&] { device.listSongs(); }, 60000);
        check(listed.completed && listed.error.isEmpty() && !listed.songs.isEmpty(), QStringLiteral("通过 Qt32/Windows Shell 读取真实头显歌曲列表"));
        if (!listed.completed || listed.songs.isEmpty()) { check(false, listed.error); return 1; }
        MtpSongEntry song;
        for (const auto &entry : listed.songs) if (entry.name.startsWith(QStringLiteral("2369d (Dry Hands"))) song = entry;
        check(!song.locator.isEmpty(), QStringLiteral("真实头显中定位 Dry Hands 测试歌曲"));
        if (song.locator.isEmpty()) return 1;
        auto copied = waitFor(device, [&] { device.importSong(song); }, 60000);
        check(copied.completed && copied.error.isEmpty() && QFileInfo::exists(copied.path + QStringLiteral("/Info.dat")), QStringLiteral("通过服务完成真实 MTP 只读复制及完整性校验"));
        if (!copied.completed) { check(false, copied.error); return 1; }
        QFile info(copied.path + QStringLiteral("/Info.dat"));
        info.open(QIODevice::ReadOnly);
        const auto object = QJsonDocument::fromJson(info.readAll()).object();
        QString audio = copied.path + QLatin1Char('/') + object.value(QStringLiteral("_songFilename")).toString();
        if (!QFileInfo::exists(audio) && audio.endsWith(QStringLiteral(".egg"), Qt::CaseInsensitive)) audio.replace(audio.size() - 4, 4, QStringLiteral(".ogg"));
        check(QFileInfo(audio).size() > 0, QStringLiteral("原 Info.dat 的 .egg 引用可匹配设备上的 .ogg 声音副本"));
        return failures ? 1 : 0;
    }
    QTemporaryDir scripts(QDir::tempPath() + QStringLiteral("/lmsc-mtp-tests-XXXXXX"));
    check(scripts.isValid(), QStringLiteral("创建测试临时目录"));
    MtpImportService service;
    service.setScriptPath(scripts.path() + QStringLiteral("/不存在.ps1"));
    auto result = waitFor(service, [&] { service.listSongs(); });
    check(!result.error.isEmpty() && !result.completed && !service.isBusy(), QStringLiteral("缺少脚本时报告失败，不报告导入成功"));

    const QString locator = QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("device"), QStringLiteral("Pico Neo 3")},
        {QStringLiteral("segments"), QJsonArray{QStringLiteral("内部共享存储空间"), QStringLiteral("歌曲 $(abc) ' 空格")}}
    }).toJson(QJsonDocument::Compact));
    const QString escapedLocator = QString(locator).replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString listScript = QStringLiteral(
        "$locator='%1' | ConvertFrom-Json\n"
        "$song=@{name='中文歌曲 $(abc)';deviceName='Pico Neo 3';location='内部共享存储空间';locator=$locator}\n"
        "[Console]::WriteLine((@{type='songs';songs=@($song)}|ConvertTo-Json -Depth 8 -Compress))\n").arg(escapedLocator);
    service.setScriptPath(makeScript(scripts, listScript));
    result = waitFor(service, [&] { service.listSongs(); });
    check(result.completed && result.songs.size() == 1 && result.songs[0].name == QStringLiteral("中文歌曲 $(abc)"), QStringLiteral("中文、空格及字面 shell 字符的列表读取"));
    check(!service.isBusy(), QStringLiteral("列表完成后释放忙碌状态"));

    MtpSongEntry entry;
    entry.name = QStringLiteral("中文歌曲 $(abc)");
    entry.deviceName = QStringLiteral("Pico Neo 3");
    entry.locator = locator;
    const QString importScript = QStringLiteral(
        "$locator=$env:LMSC_MTP_LOCATOR|ConvertFrom-Json\n"
        "if($locator.segments[1] -cne '歌曲 $(abc) '' 空格'){throw '定位信息变形'}\n"
        "if([IO.File]::ReadAllText((Join-Path $env:LMSC_MTP_SESSION '.lmsc-mtp-session')) -ne $env:LMSC_MTP_TOKEN){throw '临时目录令牌错误'}\n"
        "[IO.File]::WriteAllText((Join-Path $env:LMSC_MTP_TARGET 'Info.dat'),'{}')\n"
        "[Console]::WriteLine((@{type='imported';path=$env:LMSC_MTP_TARGET}|ConvertTo-Json -Compress))\n");
    service.setScriptPath(makeScript(scripts, importScript));
    result = waitFor(service, [&] { service.importSong(entry); });
    check(result.completed && result.error.isEmpty() && QFileInfo::exists(result.path + QStringLiteral("/Info.dat")), QStringLiteral("异步导入以 Unicode 环境变量传定位参数并保留本地临时副本"));

    service.setScriptPath(makeScript(scripts, QStringLiteral("[Console]::WriteLine('{\"type\":\"imported\",\"path\":\"C:/unexpected\"}')\n")));
    result = waitFor(service, [&] { service.importSong(entry); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("不接受与本地临时目标不符的完成结果"));

    service.setScriptPath(makeScript(scripts, QStringLiteral("[Console]::WriteLine('{\"type\":\"progress\",\"message\":\"复制中\",\"percent\":0}')\nStart-Sleep -Seconds 8\n[Console]::WriteLine('{\"type\":\"songs\",\"songs\":[]}')\n")));
    auto cancelOnProgress = QObject::connect(&service, &MtpImportService::taskProgress, &service, [&](const QString &, int percent) { if (percent == 0) service.cancel(); });
    result = waitFor(service, [&] { service.listSongs(); });
    QObject::disconnect(cancelOnProgress);
    check(result.cancelled && !result.completed && !service.isBusy(), QStringLiteral("复制过程中取消不会发出半成品成功信号"));

    auto cancelBeforeStart = QObject::connect(&service, &MtpImportService::taskProgress, &service, [&](const QString &, int) { service.cancel(); });
    result = waitFor(service, [&] { service.listSongs(); });
    QObject::disconnect(cancelBeforeStart);
    check(result.cancelled && !result.completed && !service.isBusy(), QStringLiteral("进度回调同步取消不会重新启动子进程"));

    service.setScriptPath(makeScript(scripts, QStringLiteral("[Console]::WriteLine('{\"type\":\"error\",\"message\":\"头显未连接\"}')\nexit 1\n")));
    result = waitFor(service, [&] { service.listSongs(); });
    check(result.error == QStringLiteral("头显未连接") && !result.completed && !service.isBusy(), QStringLiteral("设备失败保留中文原因并允许重试"));
    return failures ? 1 : 0;
}
