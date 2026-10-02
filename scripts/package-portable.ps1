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
$projectLicense = Join-Path $workspace 'LICENSE'
$projectChangelog = Join-Path $workspace 'CHANGELOG.md'
$appInfoHeader = Join-Path $workspace 'src/core/AppInfo.h'
foreach ($required in @($sourceExe, $deployTool, $audioPlugin,
                       (Join-Path $ffmpeg 'bin/ffmpeg.exe'), (Join-Path $ffmpeg 'bin/ffprobe.exe'),
                       $mtpScript, $projectLicense, $projectChangelog, $appInfoHeader)) {
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

双击 LightsaberMusicalScoreCreation.exe 启动。请保留同目录的 DLL、audio、platforms、imageformats 和 tools 等资源；无需安装 Qt、FFmpeg 或 Python。基础编辑可离线运行，AI 连接需要联网；Codex 账号方式需要本机安装官方 Codex CLI。

当前包面向 Windows 64 位电脑。Qt 编辑器为 32 位，随包音频工具为独立的 64 位进程。Windows 10 兼容性仍需独立验证。

1. “导入歌曲文件夹”的“电脑文件夹”页选择含 Info.dat 的本地歌曲；“导入曲谱 ZIP”打开本地 ZIP。连接并解锁 PICO、允许 USB 文件传输后，可在“PICO 头显”页刷新歌曲、选择并点击“导入选中歌曲”。歌曲先只读复制到电脑，不写入头显原文件。
2. “新歌 · MP3 / MP4”选择声音、裁剪片段与难度；自动转为 Ogg，建立一张空谱。可选简单（Easy）、普通（Normal）、困难（Hard）、专家（Expert）、专家+（ExpertPlus），创建后也能在“新歌难度”调整并保留已放置的物件。导出文件名分别为 Easy.dat、Normal.dat、Hard.dat、Expert.dat、ExpertPlus.dat。估拍后可校准 BPM 和第一拍。
3. 在 4×3 面板放置音符、炸弹或墙；支持多选、复制、粘贴、镜像、撤销/重做。受保护物件显示原因。
4. “保存工程”默认建议程序旁的 projects/<歌名>/project.lmsc；同名目录使用编号。程序目录无写权限时回退到用户文档的 光剑曲谱制作/工程。工程文件旁的 assets-*、source-* 目录与工程一起保存和移动；首次保存后自动保存恢复快照。
5. “导出歌曲目录”创建独立的新目录。新歌默认添加 2 秒开场缓冲，音频、音符、炸弹与墙同步后移，工程内编辑时间保持原样；可在导出时改为 0 秒关闭。导入的原歌曲不自动添加缓冲。导出后再手动复制整份歌曲到 PICO Neo 3 对应游戏的目录：
   《星穹绿洲》：此电脑\Pico Neo 3\内部共享存储空间\SoulTopia\BeatNote\Custom
   《光之乐团》：此电脑\Pico Neo 3\内部共享存储空间\Android\data\com.StarRiverVR.LightBand\files\CustomMusic

头显导入页会读取上述两个目录并标明游戏来源。新谱使用基础 v2.2 格式，已有 v2/v3 原数据和音频保留；新歌导出的第一拍偏移已写入物件拍数。

Ctrl+O 选择电脑或头显歌曲来源；Ctrl+S 保存；Ctrl+E 导出；Ctrl+Z 撤销；Ctrl+Shift+Z 重做；Ctrl+C/Ctrl+V 复制粘贴；Delete 删除。其余操作使用界面菜单和控制区。

用户已反馈此前版本导出的歌曲可在 PICO Neo 3 的《星穹绿洲》和《光之乐团》正常游玩。此次新增开场缓冲用于处理开头方块不显示的问题，其在两款游戏中的实际效果仍需重新导出后戴头显复测。不同难度标签在游戏中的具体映射也需分别核验。自动制谱、模组效果的完整预览与 APK 在后续阶段。

主窗口左侧导航可切换曲谱编辑、AI 识别和设置。设置包括浅色、深色、跟随系统主题，DeepSeek、Kimi、MiMo、OpenAI、通义千问和自定义 API 预设，以及系统代理或手动 HTTP 代理；“设置 → 外观设置”（Ctrl+,）也可进入。填入 API Key 后可自动获取模型，也可手动填模型名。各提供商配置分别保存，Windows 密钥使用当前账户 DPAPI 加密；设置不进入歌曲工程与导出。Codex / ChatGPT 模式通过本机官方 CLI 检查已有授权、启动浏览器登录及获取账号模型。当前只完成设置与连接，AI 识别页面尚未接入分析或自动制谱。

项目采用 GNU GPL 第 3 版，官方条款全文在根目录 LICENSE；更新记录见 CHANGELOG.md。帮助菜单的“关于光剑曲谱制作”可查看版本，再点击“查看 GPLv3 许可”阅读原文。第三方组件遵循各自许可。

Qt 5.12.12 以动态库部署，许可证和第三方声明位于 licenses/Qt。对应源码版本：
https://download.qt.io/archive/qt/5.12/5.12.12/single/qt-everywhere-src-5.12.12.zip
FFmpeg 版本、来源、构建配置和对应源码入口见 tools/ffmpeg/README.md 与 LICENSE.txt。
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
