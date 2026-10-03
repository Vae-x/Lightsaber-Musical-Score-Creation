#include "core/MtpImportService.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
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
    const QString productionScript = service.scriptPath();
    service.setScriptPath(scripts.path() + QStringLiteral("/不存在.ps1"));
    auto result = waitFor(service, [&] { service.listSongs(); });
    check(!result.error.isEmpty() && !result.completed && !service.isBusy(), QStringLiteral("缺少脚本时报告失败，不报告导入成功"));

    const QString locator = QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("device"), QStringLiteral("Pico Neo 3")},
        {QStringLiteral("segments"), QJsonArray{QStringLiteral("内部共享存储空间"), QStringLiteral("SoulTopia"),
            QStringLiteral("BeatNote"), QStringLiteral("Custom"), QStringLiteral("歌曲 $(abc) ' 空格")}}
    }).toJson(QJsonDocument::Compact));
    const QString escapedLocator = QString(locator).replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString lightBandLocator = QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("device"), QStringLiteral("Pico Neo 3")},
        {QStringLiteral("segments"), QJsonArray{QStringLiteral("内部共享存储空间"), QStringLiteral("Android"),
            QStringLiteral("data"), QStringLiteral("com.StarRiverVR.LightBand"), QStringLiteral("files"),
            QStringLiteral("CustomMusic"), QStringLiteral("同名中文歌曲 $(abc) ' 空格")}}
    }).toJson(QJsonDocument::Compact));
    const QString escapedLightBandLocator = QString(lightBandLocator).replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString listScript = QStringLiteral(
        "$locator='%1' | ConvertFrom-Json\n"
        "$lightBandLocator='%2' | ConvertFrom-Json\n"
        "$song=@{name='中文歌曲 $(abc)';deviceName='Pico Neo 3';gameId='oasis';storageName='内部共享存储空间';categorySegments=@('分类','电子音乐');location='内部共享存储空间';locator=$locator}\n"
        "$lightBandSong=@{name='中文歌曲 $(abc)';deviceName='Pico Neo 3';gameName='光之乐团';location='内部共享存储空间\\Android\\data\\com.StarRiverVR.LightBand\\files\\CustomMusic';locator=$lightBandLocator}\n"
        "[Console]::WriteLine((@{type='songs';songs=@($song,$lightBandSong)}|ConvertTo-Json -Depth 8 -Compress))\n").arg(escapedLocator, escapedLightBandLocator);
    service.setScriptPath(makeScript(scripts, listScript));
    result = waitFor(service, [&] { service.listSongs(); });
    check(result.completed && result.songs.size() == 2 && result.songs[0].name == QStringLiteral("中文歌曲 $(abc)"), QStringLiteral("中文、空格及字面 shell 字符的列表读取"));
    check(result.songs.size() == 2 && result.songs[0].gameName.isEmpty()
            && result.songs[1].gameName == QStringLiteral("光之乐团")
            && result.songs[1].locator == lightBandLocator,
          QStringLiteral("同名歌曲保留独立来源与光之乐团深层定位，兼容未提供游戏名称的列表"));
    check(!service.isBusy(), QStringLiteral("列表完成后释放忙碌状态"));
    check(result.songs.size() == 2 && result.songs[0].gameId == QStringLiteral("oasis")
            && result.songs[0].storageName == QStringLiteral("内部共享存储空间")
            && result.songs[0].categorySegments == QStringList{QStringLiteral("分类"), QStringLiteral("电子音乐")},
          QStringLiteral("分类路径、游戏标识和存储空间元数据完整保留"));

    MtpSongEntry entry;
    entry.name = QStringLiteral("中文歌曲 $(abc)");
    entry.deviceName = QStringLiteral("Pico Neo 3");
    entry.locator = locator;
    const QString importScript = QStringLiteral(
        "$locator=$env:LMSC_MTP_LOCATOR|ConvertFrom-Json\n"
        "if($locator.segments[4] -cne '歌曲 $(abc) '' 空格'){throw '定位信息变形'}\n"
        "if([IO.File]::ReadAllText((Join-Path $env:LMSC_MTP_SESSION '.lmsc-mtp-session')) -ne $env:LMSC_MTP_TOKEN){throw '临时目录令牌错误'}\n"
        "[IO.File]::WriteAllText((Join-Path $env:LMSC_MTP_TARGET 'Info.dat'),'{}')\n"
        "[Console]::WriteLine((@{type='imported';path=$env:LMSC_MTP_TARGET}|ConvertTo-Json -Compress))\n");
    service.setScriptPath(makeScript(scripts, importScript));
    result = waitFor(service, [&] { service.importSong(entry); });
    check(result.completed && result.error.isEmpty() && QFileInfo::exists(result.path + QStringLiteral("/Info.dat")), QStringLiteral("异步导入以 Unicode 环境变量传定位参数并保留本地临时副本"));

    const QString lightBandImportScript = QStringLiteral(
        "$locator=$env:LMSC_MTP_LOCATOR|ConvertFrom-Json\n"
        "if(@($locator.segments).Count -ne 7 -or $locator.segments[3] -cne 'com.StarRiverVR.LightBand' -or $locator.segments[6] -cne '同名中文歌曲 $(abc) '' 空格'){throw '光之乐团定位信息变形'}\n"
        "[IO.File]::WriteAllText((Join-Path $env:LMSC_MTP_TARGET 'Info.dat'),'{}')\n"
        "[Console]::WriteLine((@{type='imported';path=$env:LMSC_MTP_TARGET}|ConvertTo-Json -Compress))\n");
    MtpSongEntry lightBandEntry = entry;
    lightBandEntry.locator = lightBandLocator;
    service.setScriptPath(makeScript(scripts, lightBandImportScript));
    result = waitFor(service, [&] { service.importSong(lightBandEntry); });
    check(result.completed && result.error.isEmpty() && QFileInfo::exists(result.path + QStringLiteral("/Info.dat")),
          QStringLiteral("光之乐团七层定位通过环境变量完整传递并只向本地临时目录复制"));

    const QString remoteSongName = QStringLiteral("合成歌曲|示例 <节奏>:\"?* $(abc) ' . ");
    const QStringList remoteCategories{QStringLiteral("CON"), QStringLiteral("分类|尾点."), QStringLiteral("尾空格 ")};
    const QJsonObject remoteLocatorObject{
        {QStringLiteral("device"), QStringLiteral("Pico Neo 3")},
        {QStringLiteral("segments"), QJsonArray{QStringLiteral("内部共享存储空间"), QStringLiteral("SoulTopia"),
            QStringLiteral("BeatNote"), QStringLiteral("Custom"), remoteCategories[0], remoteCategories[1],
            remoteCategories[2], remoteSongName}}
    };
    const QString remoteLocator = QString::fromUtf8(QJsonDocument(remoteLocatorObject).toJson(QJsonDocument::Compact));
    const QString remoteListRecord = QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("songs")},
        {QStringLiteral("songs"), QJsonArray{QJsonObject{
            {QStringLiteral("name"), remoteSongName}, {QStringLiteral("deviceName"), QStringLiteral("Pico Neo 3")},
            {QStringLiteral("categorySegments"), QJsonArray::fromStringList(remoteCategories)},
            {QStringLiteral("locator"), remoteLocatorObject}}}}
    }).toJson(QJsonDocument::Compact));
    service.setScriptPath(makeScript(scripts, QStringLiteral("[Console]::WriteLine('%1')\n")
        .arg(QString(remoteListRecord).replace(QLatin1Char('\''), QStringLiteral("''")))));
    result = waitFor(service, [&] { service.listSongs(); });
    check(result.completed && result.songs.size() == 1 && result.songs[0].name == remoteSongName
            && result.songs[0].categorySegments == remoteCategories && result.songs[0].locator == remoteLocator,
          QStringLiteral("头显外层目录的管道符、保留名与尾点尾空格原样保留在列表和定位信息中"));
    MtpSongEntry remoteEntry = entry;
    remoteEntry.name = remoteSongName;
    remoteEntry.locator = remoteLocator;
    service.setScriptPath(makeScript(scripts, QStringLiteral(
        "if($env:LMSC_MTP_LOCATOR -cne '%1'){throw '远程目录 JSON 定位信息变形'}\n"
        "if([IO.Path]::GetFileName($env:LMSC_MTP_TARGET) -cne 'song'){throw '本机目标误用远程目录名称'}\n"
        "[IO.File]::WriteAllText((Join-Path $env:LMSC_MTP_TARGET 'Info.dat'),'{}')\n"
        "[Console]::WriteLine((@{type='imported';path=$env:LMSC_MTP_TARGET}|ConvertTo-Json -Compress))\n")
        .arg(QString(remoteLocator).replace(QLatin1Char('\''), QStringLiteral("''")))));
    result = waitFor(service, [&] { service.importSong(remoteEntry); });
    check(result.completed && result.error.isEmpty() && QFileInfo::exists(result.path + QStringLiteral("/Info.dat")),
          QStringLiteral("Windows 非法外层目录名通过原始定位导入固定 song 副本，无需修改头显文件名"));

    const QJsonArray rootSegments{QStringLiteral("内部共享存储空间"), QStringLiteral("SoulTopia"),
        QStringLiteral("BeatNote"), QStringLiteral("Custom")};
    const QStringList unsafeRemoteNames{QString(), QStringLiteral(" \t"), QStringLiteral("."), QStringLiteral(".."),
        QStringLiteral("分类/逃逸"), QStringLiteral("分类\\逃逸"), QStringLiteral("控制") + QChar(0),
        QStringLiteral("控制") + QChar(31), QStringLiteral("控制") + QChar(127), QStringLiteral("控制") + QChar(159)};
    for (const auto &name : unsafeRemoteNames) {
        QJsonArray invalidSegments = rootSegments;
        invalidSegments.append(name);
        MtpSongEntry invalidEntry = entry;
        invalidEntry.locator = QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("device"), QStringLiteral("Pico Neo 3")}, {QStringLiteral("segments"), invalidSegments}
        }).toJson(QJsonDocument::Compact));
        result = waitFor(service, [&] { service.importSong(invalidEntry); });
        check(!result.completed && !result.error.isEmpty() && !service.isBusy(),
              QStringLiteral("远程定位拒绝空名、相对导航、路径分隔符及控制字符（序号 %1）").arg(unsafeRemoteNames.indexOf(name)));
    }
    QList<QJsonArray> invalidRemotePaths;
    QJsonArray foreignRoot = rootSegments; foreignRoot[1] = QStringLiteral("OtherGame"); foreignRoot.append(QStringLiteral("歌曲"));
    invalidRemotePaths.append(foreignRoot);
    invalidRemotePaths.append(rootSegments);
    QJsonArray nonString = rootSegments; nonString.append(42); invalidRemotePaths.append(nonString);
    QJsonArray tooDeep = rootSegments;
    for (int depth = 0; depth < 21; ++depth) tooDeep.append(QStringLiteral("层"));
    invalidRemotePaths.append(tooDeep);
    for (const auto &segments : invalidRemotePaths) {
        MtpSongEntry invalidEntry = entry;
        invalidEntry.locator = QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("device"), QStringLiteral("Pico Neo 3")}, {QStringLiteral("segments"), segments}
        }).toJson(QJsonDocument::Compact));
        result = waitFor(service, [&] { service.importSong(invalidEntry); });
        check(!result.completed && !result.error.isEmpty() && !service.isBusy(),
              QStringLiteral("远程定位仍限制已支持的歌曲根、歌曲节点、字符串段与 20 层边界"));
    }

    const QString locatorValidationScript = QStringLiteral(
        "$tokens=$null; $parseErrors=$null\n"
        "$ast=[Management.Automation.Language.Parser]::ParseFile('%1',[ref]$tokens,[ref]$parseErrors)\n"
        "if($parseErrors.Count){throw '设备导入脚本语法错误'}\n"
        "foreach($functionName in @('Assert-SafeName','Assert-RemoteName','Get-SongSources','Test-SupportedLocator','Find-CategorizedSongs')){\n"
        "  $definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName},$true)\n"
        "  if(-not $definition){throw '缺少设备目录校验函数'}\n"
        "  Invoke-Expression $definition.Extent.Text\n"
        "}\n"
        "$lightBand='%2'|ConvertFrom-Json\n"
        "$oasis=@{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom','歌曲')}\n"
        "$categorized=@{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom','光剑曲谱制作','电子音乐','歌曲')}\n"
        "if(-not (Test-SupportedLocator $lightBand) -or -not (Test-SupportedLocator $oasis) -or -not (Test-SupportedLocator $categorized)){throw '已支持的游戏目录遭拒绝'}\n"
        "foreach($name in @('歌曲|示例','CON','尾点.','尾空格 ')){\n"
        "  $remote=@{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom',$name)}\n"
        "  if(-not (Test-SupportedLocator $remote)){throw '合法远程目录遭拒绝'}\n"
        "  $localRejected=$false; try {Assert-SafeName $name}catch{$localRejected=$true}\n"
        "  if(-not $localRejected){throw '本地资源文件名保护被放宽'}\n"
        "}\n"
        "$invalid=@(\n"
        "  @{device='Pico Neo 3';segments=@('内部共享存储空间','Android','data','com.other.game','files','CustomMusic','歌曲')},\n"
        "  @{device='Pico Neo 3';segments=@('内部共享存储空间','Android','data','com.StarRiverVR.LightBand','files','CustomMusic')},\n"
        "  @{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom','')},\n"
        "  @{device='';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom','歌曲')},\n"
        "  @{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom','..','歌曲')},\n"
        "  @{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom','分类/逃逸','歌曲')},\n"
        "  @{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom','分类\\逃逸','歌曲')},\n"
        "  @{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom',('控制'+[char]127),'歌曲')},\n"
        "  @{device='Pico Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom')+(@('层')*21)+@('歌曲')}\n"
        ")\n"
        "foreach($item in $invalid){if(Test-SupportedLocator $item){throw '未支持的路径通过了校验'}}\n"
        "function Mock-Folder($name,$children){\n"
        "  $folder=[pscustomobject]@{Children=@($children)}; $folder|Add-Member ScriptMethod Items {return $this.Children}\n"
        "  $item=[pscustomobject]@{Name=$name;IsFolder=$true;Folder=$folder}; $item|Add-Member ScriptProperty GetFolder {return $this.Folder}; return $item\n"
        "}\n"
        "$info=[pscustomobject]@{Name='Info.dat';IsFolder=$false}\n"
        "$resource=Mock-Folder '资源' @($info)\n"
        "$song=Mock-Folder '歌曲' @($info,$resource)\n"
        "$category=Mock-Folder '电子音乐' @($song)\n"
        "$root=Mock-Folder 'Custom' @($category)\n"
        "$source=@(Get-SongSources)[0]; $device=@{Name='Pico Neo 3'}; $storage=@{Name='内部共享存储空间'}\n"
        "$songs=New-Object 'Collections.Generic.List[object]'; $counters=@{Songs=0;Directories=0}\n"
        "Find-CategorizedSongs $root.GetFolder @() 0 $source $device $storage $songs $counters\n"
        "if($songs.Count -ne 1 -or $songs[0].categorySegments[0] -cne '电子音乐' -or @($songs[0].locator.segments).Count -ne 6 -or $counters.Directories -ne 3){throw '递归分类或歌曲资源止步错误'}\n"
        "$empty=Mock-Folder '空目录' @(); $emptySongs=New-Object 'Collections.Generic.List[object]'\n"
        "Find-CategorizedSongs $empty.GetFolder @() 0 $source $device $storage $emptySongs @{Songs=0;Directories=0}\n"
        "if($emptySongs.Count -ne 0){throw '空目录误识别为歌曲'}\n"
        "foreach($limit in @('depth','songs','directories')){\n"
        "  $rejected=$false; try {\n"
        "    if($limit -eq 'depth'){Find-CategorizedSongs $empty.GetFolder @('歌曲') 21 $source $device $storage $emptySongs @{Songs=0;Directories=0}}\n"
        "    if($limit -eq 'songs'){Find-CategorizedSongs $song.GetFolder @('歌曲') 1 $source $device $storage $emptySongs @{Songs=4000;Directories=0}}\n"
        "    if($limit -eq 'directories'){Find-CategorizedSongs $empty.GetFolder @() 0 $source $device $storage $emptySongs @{Songs=0;Directories=10000}}\n"
        "  }catch{$rejected=$true}; if(-not $rejected){throw ('扫描上限未拒绝：'+$limit)}\n"
        "}\n"
        "[Console]::WriteLine('{\"type\":\"songs\",\"songs\":[]}')\n")
        .arg(QString(productionScript).replace(QLatin1Char('\''), QStringLiteral("''")), escapedLightBandLocator);
    service.setScriptPath(makeScript(scripts, locatorValidationScript));
    result = waitFor(service, [&] { service.listSongs(); });
    check(result.completed && result.error.isEmpty(),
          QStringLiteral("真实脚本递归分类、歌曲资源止步、三种扫描上限和安全目录白名单通过"));

    MtpSongEntry gameRoot = entry; gameRoot.isSong = false;
    result = waitFor(service, [&] { service.importSong(gameRoot); });
    check(!result.completed && !result.error.isEmpty(), QStringLiteral("空游戏根与分类节点不能作为歌曲导入"));
    service.setScriptPath(makeScript(scripts, QStringLiteral("[Console]::WriteLine('{\"type\":\"songs\",\"songs\":[{\"name\":\"星穹绿洲\",\"deviceName\":\"Pico Neo 3\",\"gameId\":\"oasis\",\"isSong\":false,\"locator\":{\"device\":\"Pico Neo 3\",\"segments\":[\"内部共享存储空间\",\"SoulTopia\",\"BeatNote\",\"Custom\"]}}]}')\n")));
    result = waitFor(service, [&] { service.listSongs(); });
    check(result.completed && result.songs.size() == 1 && !result.songs.first().isSong,
          QStringLiteral("空游戏歌曲根作为浏览节点保留，完整扫描正常成功"));

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
