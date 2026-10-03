param(
    [string]$BuildDirectory = 'build/release',
    [string]$QtDirectory = 'F:/Qt/Qt5.12.12/5.12.12/mingw73_32',
    [string]$CompilerDirectory = 'F:/Qt/Qt5.12.12/Tools/mingw730_32/bin'
)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$distRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'dist'))
$workspacePrefix = $workspace.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (!$distRoot.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output path escaped the workspace.'
}
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else { [IO.Path]::GetFullPath((Join-Path $workspace $BuildDirectory)) }
if (!$buildRoot.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build directory must be inside this workspace.'
}
$sourceExe = Join-Path $buildRoot 'src/src.exe'
$deployTool = Join-Path $QtDirectory 'bin/windeployqt.exe'
$audioPlugin = Join-Path $QtDirectory 'plugins/audio/qtaudio_windows.dll'
$ffmpeg = Join-Path $workspace 'third_party/ffmpeg'
$mtpScript = Join-Path $workspace 'scripts/windows/mtp-import.ps1'
$wpdHelper = Join-Path $buildRoot 'src/tools/mtp/WpdTransfer.exe'
$projectLicense = Join-Path $workspace 'LICENSE'
$projectChangelog = Join-Path $workspace 'CHANGELOG.md'
$appInfoHeader = Join-Path $workspace 'src/core/AppInfo.h'
foreach ($required in @($sourceExe, $deployTool, $audioPlugin,
                       (Join-Path $ffmpeg 'bin/ffmpeg.exe'), (Join-Path $ffmpeg 'bin/ffprobe.exe'),
                       $mtpScript, $wpdHelper, $projectLicense, $projectChangelog, $appInfoHeader)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing required runtime: $required" }
}
$buildCache = Get-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Raw
if ($buildCache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\s*$') {
    throw 'Portable packaging requires a Release build.'
}
$versionMatches = [regex]::Matches([IO.File]::ReadAllText($appInfoHeader, [Text.Encoding]::UTF8),
    'inline\s+QString\s+version\(\)\s*\{\s*return\s+QStringLiteral\("(?<Version>\d+\.\d+\.\d+)"\);\s*\}')
if ($versionMatches.Count -ne 1) { throw 'Unable to read the application version from AppInfo.h.' }
$applicationVersion = $versionMatches[0].Groups['Version'].Value
$expectedBinaryVersion = [Version]($applicationVersion + '.0')
$exeVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($sourceExe)
$binaryFileVersion = [Version]::new($exeVersion.FileMajorPart, $exeVersion.FileMinorPart,
                                  $exeVersion.FileBuildPart, $exeVersion.FilePrivatePart)
$binaryProductVersion = [Version]::new($exeVersion.ProductMajorPart, $exeVersion.ProductMinorPart,
                                     $exeVersion.ProductBuildPart, $exeVersion.ProductPrivatePart)
if ($exeVersion.FileVersion -ne $applicationVersion -or $exeVersion.ProductVersion -ne $applicationVersion -or
    $binaryFileVersion -ne $expectedBinaryVersion -or $binaryProductVersion -ne $expectedBinaryVersion) {
    throw "Executable version does not match AppInfo.h ($applicationVersion). Rebuild the Release application before packaging."
}

# Every package gets a new directory. Existing packages are retained for review.
$package = Join-Path $distRoot '光剑曲谱制作'
if (Test-Path -LiteralPath $package) {
    $base = Join-Path $distRoot ('光剑曲谱制作-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $package = $base
    $suffix = 1
    while (Test-Path -LiteralPath $package) { $package = "$base-$suffix"; ++$suffix }
}
if (!$package.StartsWith($distRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar,
                        [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid package target.' }
[IO.Directory]::CreateDirectory($package) | Out-Null
$targetExe = Join-Path $package 'LightsaberMusicalScoreCreation.exe'
Copy-Item -LiteralPath $sourceExe -Destination $targetExe
Copy-Item -LiteralPath $projectLicense, $projectChangelog -Destination $package
[IO.Directory]::CreateDirectory((Join-Path $package 'projects')) | Out-Null

$taskPreviousPath = $env:PATH
try {
    $env:PATH = $CompilerDirectory + ';' + (Join-Path $QtDirectory 'bin') + ';' + $taskPreviousPath
    & $deployTool --release --no-translations --compiler-runtime $targetExe
    if ($LASTEXITCODE -ne 0) { throw "Qt runtime deployment failed: $LASTEXITCODE" }
} finally { $env:PATH = $taskPreviousPath }

$audioDirectory = Join-Path $package 'audio'
[IO.Directory]::CreateDirectory($audioDirectory) | Out-Null
Copy-Item -LiteralPath $audioPlugin -Destination $audioDirectory -Force
$tools = Join-Path $package 'tools/ffmpeg'
[IO.Directory]::CreateDirectory($tools) | Out-Null
Get-ChildItem -LiteralPath (Join-Path $ffmpeg 'bin') -File | ForEach-Object {
    if ($_.Extension -in @('.exe', '.dll')) { Copy-Item -LiteralPath $_.FullName -Destination $tools }
}
Copy-Item -LiteralPath (Join-Path $ffmpeg 'LICENSE.txt'), (Join-Path $ffmpeg 'README.md') -Destination $tools
$mtpDirectory = Join-Path $package 'tools/mtp'
[IO.Directory]::CreateDirectory($mtpDirectory) | Out-Null
Copy-Item -LiteralPath $mtpScript -Destination $mtpDirectory
Copy-Item -LiteralPath $wpdHelper -Destination $mtpDirectory

# License texts come from the exact locally installed Qt 5.12.12 source tree.
$qtSources = [IO.Path]::GetFullPath((Join-Path $QtDirectory '../Src'))
$licenseDirectory = Join-Path $package 'licenses/Qt'
[IO.Directory]::CreateDirectory($licenseDirectory) | Out-Null
foreach ($name in @('LICENSE.LGPLv3', 'LICENSE.LGPLv21', 'LICENSE.GPLv3', 'LICENSE.GPLv2', 'LICENSE.FDL')) {
    $sourceLicense = Join-Path $qtSources $name
    if (!(Test-Path -LiteralPath $sourceLicense -PathType Leaf)) { throw "Qt license text missing: $sourceLicense" }
    Copy-Item -LiteralPath $sourceLicense -Destination $licenseDirectory
}
foreach ($module in @('qtbase', 'qtmultimedia', 'qtsvg', 'qtimageformats')) {
    $moduleRoot = Join-Path $qtSources $module
    $thirdParty = Join-Path $moduleRoot 'src/3rdparty'
    if (Test-Path -LiteralPath $thirdParty -PathType Container) {
        Get-ChildItem -LiteralPath $thirdParty -Recurse -File | Where-Object {
            $_.Name -match '^(LICENSE|COPYING|COPYRIGHT|NOTICE|AUTHORS|PATENTS|qt_attribution)' -or
            $_.Name -in @('README', 'README.md', 'README.txt')
        } | ForEach-Object {
            $relative = $_.FullName.Substring($moduleRoot.Length).TrimStart('\', '/')
            $target = Join-Path (Join-Path $licenseDirectory $module) $relative
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $target
        }
    }
}

$readme = @'
# 光剑曲谱制作 {{APP_VERSION}}

完整解压后双击 LightsaberMusicalScoreCreation.exe。保留所有 DLL、audio、platforms、imageformats 和 tools；基础编辑与音频处理无需安装 Qt、Python 或 FFmpeg，可以离线使用。发行包面向 Windows 64 位电脑；Qt 编辑器为 32 位，随包 FFmpeg 与 Windows WPD 传输工具为 64 位，设备工具使用系统 .NET Framework。Windows 10 兼容性仍须独立验证。

1. “导入歌曲”统一选择电脑文件夹或 ZIP，自动识别；“打开编辑工程”继续打开 .lmsc。连接并解锁 PICO、允许 USB 文件传输后，可在 PICO 页按设备、游戏、分类、歌曲展开或搜索。只读复制选中歌曲到电脑，不修改头显原文件。
2. “新歌 · MP3 / MP4”选择音轨、裁剪、难度并转换 Ogg。可选 Easy、Normal、Hard、Expert、ExpertPlus；手动编辑支持普通红蓝方块、方向、炸弹、墙、多选、复制粘贴、镜像、撤销重做与试听控制。
3. “AI 分析与制谱”先本地提取音乐特征，通过已配置 API 或本机 Codex / ChatGPT 授权请求模型规划整曲、分乐句生成；候选谱校验后可试听，再应用到所选难度。其他难度保留，支持有限自动恢复、诊断与手动继续。原音频不上传；模型请求可能产生服务费用。账号模式需本机官方 Codex CLI。
4. “保存工程”默认使用程序旁 projects/<歌名>/project.lmsc；程序目录不可写时回退到用户文档“光剑曲谱制作/工程”。工程与 assets-*、source-* 和恢复快照一起保留和移动。assets-* 是原始快照，直接复制它不会包含后续编辑。
5. “导出歌曲”先生成完整电脑副本，位置为所选目录/光剑曲谱制作/歌曲名-by光剑曲谱/，同名追加编号，不覆盖。完成后可打开歌曲目录，手动复制整个目录到设备。
6. 连接 PICO 后也可选择直接导出到目标游戏。仅在游戏既有歌曲根目录的“光剑曲谱制作”分类中新建歌曲目录，逐文件上传并回读 SHA256，Info.dat 最后上传并验证引用。不修改原歌曲、成绩、收藏或配置，不创建缺失的游戏根。取消或断连保留电脑副本，提示可能残留的新设备目录，不自动删除。

内部共享存储空间的歌曲根目录：
- 星穹绿洲：SoulTopia/BeatNote/Custom
- 光之乐团：Android/data/com.StarRiverVR.LightBand/files/CustomMusic

两处新增歌曲都放入 光剑曲谱制作/歌曲名-by光剑曲谱/。光之乐团分类依据用户反馈提供；星穹绿洲分类读取未有可靠公开说明，需在游戏内验证。以前导出歌曲两游戏可用的用户反馈、电脑格式检查、设备传输和新增功能的实际游玩分别记录。

新歌导出每张谱分别命名 Easy.dat、Normal.dat、Hard.dat、Expert.dat、ExpertPlus.dat。开场缓冲默认 2 秒，0～10 秒可调，0 关闭；所有新歌难度的基础物件同步后移，音频只添加一次静音，工程原时间保持不变。已有导入歌曲不自动加缓冲，原谱面文件名和音频保留。缓冲效果与 AI 生成手感仍待两游戏实测。

布尔型 ChroMapper 书签设置不会再误判为未知变速。真正未知的时间扩展或高级关联仍保护；提示提供具体字段，另存工程保留原数据所以不会解除保护。弧线、链条、灯光及模组效果侧重保留，尚无完整预览。APK 留在后续阶段。

主窗口导航提供编辑、AI 和设置；账号授权位于大语言模型子菜单。支持浅色、深色、跟随系统，以及 API 提供商、自定义接口、代理和关于。API 密钥由当前 Windows 账户 DPAPI 加密，不进入工程或歌曲导出；便携包不含用户歌曲、工程、设置或账号凭据。

Ctrl+O 导入歌曲；Ctrl+S 保存工程；Ctrl+E 导出歌曲；Ctrl+Z / Ctrl+Shift+Z 撤销重做。其余操作使用菜单和界面控制区。

本项目采用 GNU GPL 第 3 版，官方许可全文见根目录 LICENSE，更新与验证范围见 CHANGELOG.md。Windows WPD helper 的对应源码随本版本源码发布。Qt 与 FFmpeg 及依赖各自保留原协议：licenses/Qt、tools/ffmpeg/README.md 和 LICENSE.txt。Qt 5.12.12 通过可替换动态库部署，对应源码：
https://download.qt.io/archive/qt/5.12/5.12.12/single/qt-everywhere-src-5.12.12.zip
'@
$readme = $readme.Replace('{{APP_VERSION}}', $applicationVersion)
[IO.File]::WriteAllText((Join-Path $package '使用说明.md'), $readme, [Text.UTF8Encoding]::new($true))
$licenseNotice = @'
Qt runtime: Qt 5.12.12, dynamically linked. License texts and source dependency notices are provided in this directory.
Exact Qt source archive: https://download.qt.io/archive/qt/5.12/5.12.12/single/qt-everywhere-src-5.12.12.zip
This package includes replaceable Qt DLLs; changing those libraries does not require rebuilding the application.
FFmpeg license/configuration/source references: ../../tools/ffmpeg/README.md and LICENSE.txt.
'@
[IO.File]::WriteAllText((Join-Path $licenseDirectory 'README.txt'), $licenseNotice, [Text.UTF8Encoding]::new($false))

$inventory = Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
    [pscustomobject]@{
        File = $_.FullName.Substring($package.Length).TrimStart('\', '/').Replace('\', '/')
        Bytes = $_.Length
        Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest = [pscustomobject]@{
    Application = '光剑曲谱制作'
    Version = $applicationVersion
    ExecutableFileVersion = $exeVersion.FileVersion
    ExecutableProductVersion = $exeVersion.ProductVersion
    BuiltUtc = (Get-Date).ToUniversalTime().ToString('o')
    Configuration = 'Release'
    Qt = '5.12.12 / MinGW 7.3 32-bit'
    Files = @($inventory)
}
[IO.File]::WriteAllText((Join-Path $package 'package-manifest.json'), ($manifest | ConvertTo-Json -Depth 5),
                      [Text.UTF8Encoding]::new($false))
Write-Output "Portable package: $package"
